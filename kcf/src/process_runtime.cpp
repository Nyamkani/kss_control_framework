#include "kcf/process/process_runtime.hpp"

#include <chrono>
#include <cmath>
#include <thread>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{

// One active ProcessRuntime per process.
volatile sig_atomic_t stop_requested = 0;

void HandleStopSignal(int)
{
    stop_requested = 1;
}

} // namespace

namespace kcf
{

int ProcessRuntime::Run(ProcessElement& element)
{
    state_ = ProcessState::STARTING;

    loop_heartbeat_ = 0;
    runtime_error_ = 0;
    int initialized = Initialize();
    const int supervised = StartSupervision();
    if (!initialized) initialized = supervised;
    if (initialized != 0)
    {
        running_ = false;
        runtime_error_ = initialized;
        state_ = ProcessState::ERROR;
        Finalize();
        return initialized;
    }

    int setup_result;
    try
    {
        setup_result = element.Setup();
    }
    catch (...)
    {
        setup_result = -EFAULT;
    }

    if (setup_result != 0)
    {
        running_ = false;
        runtime_error_ = setup_result;
        state_ = ProcessState::ERROR;
        Finalize();
        return setup_result;
    }

    running_ = true;
    state_ = ProcessState::RUNNING;

    int result = 0;
    try
    {
        using Clock = std::chrono::steady_clock;
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / loop_frequency_hz_));
        auto next_tick = Clock::now();

        while (running_)
        {
            if (stop_requested != 0)
            {
                RequestStop();
            }
            if (!running_)
            {
                break;
            }

            element.Loop();
            if (loop_heartbeat_.load() == std::numeric_limits<std::uint64_t>::max())
            {
                result = -EOVERFLOW;
                break;
            }
            loop_heartbeat_.fetch_add(1);

            if (stop_requested != 0)
            {
                RequestStop();
            }
            if (!running_)
            {
                break;
            }

            next_tick += period;
            const auto now = Clock::now();
            if (now < next_tick)
            {
                std::this_thread::sleep_until(next_tick);
            }
            else
            {
                // Discard missed ticks instead of running catch-up iterations.
                next_tick = now;
            }
        }
    }
    catch (...)
    {
        result = -EFAULT;
        running_ = false;
    }

    runtime_error_ = result;
    state_ = result ? ProcessState::ERROR : ProcessState::STOPPING;
    try
    {
        element.Shutdown();
    }
    catch (...)
    {
        result = -EFAULT;
    }

    running_ = false;
    runtime_error_ = result;
    state_ = result == 0 ? ProcessState::STOPPED : ProcessState::ERROR;
    Finalize();
    return result;
}

void ProcessRuntime::RequestStop()
{
    running_ = false;
}

bool ProcessRuntime::IsRunning() const
{
    return running_.load();
}

ProcessState ProcessRuntime::GetState() const
{
    return state_.load();
}

void ProcessRuntime::SetLoopFrequency(double hz)
{
    if (!std::isfinite(hz) || hz <= 0.0)
    {
        return;
    }

    // Require a representable, nonzero steady_clock period.
    using Clock = std::chrono::steady_clock;
    const auto seconds = std::chrono::duration<long double>(1.0L / hz);
    if (seconds < Clock::duration{1} || seconds >= Clock::duration::max() / 2)
    {
        return;
    }

    loop_frequency_hz_ = hz;
}

int ProcessRuntime::Initialize()
{
    stop_requested = 0;

    struct sigaction action {};
    action.sa_handler = HandleStopSignal;
    if (sigemptyset(&action.sa_mask) != 0)
    {
        return -errno;
    }

    if (sigaction(SIGINT, &action, &previous_sigint_) != 0)
    {
        return -errno;
    }
    sigint_installed_ = true;

    if (sigaction(SIGTERM, &action, &previous_sigterm_) != 0)
    {
        return -errno;
    }
    sigterm_installed_ = true;
    return 0;
}

int ProcessRuntime::StartSupervision()
{
    const char* text = std::getenv(SUPERVISION_FD_ENV);
    if (!text) return 0; // standalone: no socket and no worker
    int fd = -1;
    const auto length = std::strlen(text);
    const auto parsed = std::from_chars(text, text + length, fd);
    if (parsed.ec != std::errc{} || parsed.ptr != text + length || fd < 0)
    { unsetenv(SUPERVISION_FD_ENV); return -EINVAL; }
    unsetenv(SUPERVISION_FD_ENV);
    supervision_fd_ = fd;
    int type=0; socklen_t size=sizeof(type);
    sockaddr_un address{}; socklen_t address_size=sizeof(address);
    if (getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_SEQPACKET ||
        getsockname(fd,reinterpret_cast<sockaddr*>(&address),&address_size) || address.sun_family!=AF_UNIX)
        return -EPROTO;
    const int flags=fcntl(fd,F_GETFD);
    if (flags<0 || fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0) return -errno;
    supervision_running_ = true;
    try { supervision_worker_=std::thread(&ProcessRuntime::Supervise,this); }
    catch (const std::system_error& error) { supervision_running_=false; return -error.code().value(); }
    catch (...) { supervision_running_=false; return -ENOMEM; }
    return 0;
}
void ProcessRuntime::Supervise()
{
    while (supervision_running_.load())
    {
        pollfd event{supervision_fd_,POLLIN,0};
        const int ready=poll(&event,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (event.revents&(POLLERR|POLLNVAL))) break;
        if (!ready) continue;
        if (!(event.revents&POLLIN)) { if (event.revents&POLLHUP) break; continue; }
        RuntimeStatusRequest request{};
        const auto size=recv(supervision_fd_,&request,sizeof(request),MSG_DONTWAIT|MSG_TRUNC);
        if (size<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (size<=0) break;
        if (size!=sizeof(request) || request.magic!=RUNTIME_STATUS_MAGIC ||
            request.protocol_version!=RUNTIME_STATUS_VERSION || request.packet_type!=1 || !request.request_id) continue;
        RuntimeStatusResponse response{};
        response.request_id=request.request_id;
        response.pid=getpid();
        response.state=state_.load();
        response.loop_heartbeat=loop_heartbeat_.load();
        response.runtime_error=runtime_error_.load();
        const auto sent=send(supervision_fd_,&response,sizeof(response),MSG_DONTWAIT|MSG_NOSIGNAL);
        if (sent<0 && errno!=EAGAIN && errno!=EINTR) break;
    }
    supervision_running_=false;
}

void ProcessRuntime::Finalize()
{
    supervision_running_ = false;
    if (supervision_fd_ >= 0) shutdown(supervision_fd_, SHUT_RDWR);
    if (supervision_worker_.joinable()) supervision_worker_.join();
    if (supervision_fd_ >= 0) close(supervision_fd_);
    supervision_fd_ = -1;
    if (sigterm_installed_)
    {
        sigaction(SIGTERM, &previous_sigterm_, nullptr);
        sigterm_installed_ = false;
    }
    if (sigint_installed_)
    {
        sigaction(SIGINT, &previous_sigint_, nullptr);
        sigint_installed_ = false;
    }
}

} // namespace kcf
