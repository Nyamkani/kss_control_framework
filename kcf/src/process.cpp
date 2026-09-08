#include "kcf/process/process.hpp"

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

extern char** environ;

namespace kcf
{

int Process::Start(const std::string& executable, const std::vector<std::string>& args)
{
    if (pid_ > 0)
    {
        const int result = CollectExit(WNOHANG);
        if (result <= 0) return result == 0 ? -EBUSY : result;
    }
    if (executable.empty() || executable.find('\0') != std::string::npos) return -EINVAL;
    // Prepare argv before fork; the child only uses async-signal-safe syscalls.
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : args)
    {
        if (arg.find('\0') != std::string::npos) return -EINVAL;
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    // Environment storage is prepared before fork. Preserve user variables and
    // replace only the framework's internal FD entry.
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry)
        if (std::strncmp(*entry, "KCF_SUPERVISION_FD=", 19) != 0) environment.emplace_back(*entry);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) != 0) return -errno;
    try { environment.emplace_back(std::string(SUPERVISION_FD_ENV) + "=" + std::to_string(sockets[1])); }
    catch (...) { close(sockets[0]); close(sockets[1]); throw; }
    std::vector<char*> envp;
    try
    {
        envp.reserve(environment.size() + 1);
        for (auto& value : environment) envp.push_back(value.data());
        envp.push_back(nullptr);
    }
    catch (...) { close(sockets[0]); close(sockets[1]); throw; }
    int pipe_fd[2];
    if (pipe2(pipe_fd, O_CLOEXEC) != 0) { int e=errno; close(sockets[0]); close(sockets[1]); return -e; }
    const char* path = executable.c_str();
    char* const* arguments = argv.data();
    char* const* child_environment = envp.data();
    const pid_t child = fork();
    if (child < 0)
    {
        const int error = errno;
        close(pipe_fd[0]); close(pipe_fd[1]);
        close(sockets[0]); close(sockets[1]);
        return -error;
    }
    if (child == 0)
    {
        close(pipe_fd[0]);
        close(sockets[0]);
        if (fcntl(sockets[1], F_SETFD, 0) == 0)
            execve(path, arguments, child_environment);
        const int error = errno;
        const char* data = reinterpret_cast<const char*>(&error);
        std::size_t sent = 0;
        while (sent < sizeof(error))
        {
            const auto n = write(pipe_fd[1], data + sent, sizeof(error) - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        _exit(127);
    }
    close(pipe_fd[1]);
    close(sockets[1]);
    CloseSupervision();
    supervision_fd_ = sockets[0];
    pid_ = child;
    exited_ = false;
    exit_status_ = 0;
    int exec_error = 0;
    std::size_t received = 0;
    int read_error = 0;
    while (received < sizeof(exec_error))
    {
        const auto n = read(pipe_fd[0], reinterpret_cast<char*>(&exec_error) + received,
                            sizeof(exec_error) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { read_error = errno; break; }
        if (n == 0) break; // CLOEXEC closes the writer on successful exec.
        received += static_cast<std::size_t>(n);
    }
    close(pipe_fd[0]);
    if (read_error || received)
    {
        if (read_error || received != sizeof(exec_error)) RequestStop();
        Wait();
        return -(read_error ? read_error : received == sizeof(exec_error) ? exec_error : EIO);
    }
    return 0;
}

Process::~Process() { CloseSupervision(); }
void Process::CloseSupervision()
{
    if (supervision_fd_ >= 0) close(supervision_fd_);
    supervision_fd_ = -1;
    pending_request_ = 0;
}
int Process::RequestRuntimeStatus(std::uint64_t request_id)
{
    if (supervision_fd_ < 0) return -EBADF;
    if (!request_id) return -EINVAL;
    if (pending_request_) return -EBUSY;
    RuntimeStatusRequest request{};
    request.request_id = request_id;
    const auto size = send(supervision_fd_, &request, sizeof(request), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (size < 0) return -errno;
    if (size != sizeof(request)) return -EIO;
    pending_request_ = request_id;
    return 0;
}
int Process::ReceiveRuntimeStatus(RuntimeStatusResponse& response)
{
    if (supervision_fd_ < 0) return -EBADF;
    RuntimeStatusResponse value{};
    const auto size = recv(supervision_fd_, &value, sizeof(value), MSG_DONTWAIT | MSG_TRUNC);
    if (size < 0) return -errno;
    if (!size) return -ECONNRESET;
    if (size != sizeof(value) || value.magic != RUNTIME_STATUS_MAGIC ||
        value.protocol_version != RUNTIME_STATUS_VERSION || value.packet_type != 2 ||
        value.pid != pid_ || value.state > ProcessState::ERROR || value.reserved1 ||
        value.reserved[0] || value.reserved[1] || value.reserved[2]) return -EPROTO;
    if (!pending_request_ || value.request_id != pending_request_) return -ESTALE;
    pending_request_ = 0;
    response = value;
    return 0;
}

ProcessExitInfo Process::GetExitInfo() const
{
    ProcessExitInfo info{};
    if (!exited_) return info;
    info.available = true;
    info.exited_normally = WIFEXITED(exit_status_);
    if (info.exited_normally) info.exit_code = WEXITSTATUS(exit_status_);
    info.signaled = WIFSIGNALED(exit_status_);
    if (info.signaled) info.signal_number = WTERMSIG(exit_status_);
    return info;
}

int Process::RequestStop()
{
    if (pid_ <= 0)
    {
        return -ESRCH;
    }

    const int result = CollectExit(WNOHANG);
    if (result != 0)
    {
        return result < 0 ? result : -ESRCH;
    }

    if (kill(pid_, SIGTERM) != 0)
    {
        return -errno;
    }
    return 0;
}

int Process::ForceStop()
{
    if (pid_ <= 0) return -ESRCH;
    const int result = CollectExit(WNOHANG);
    if (result != 0) return result < 0 ? result : -ESRCH;
    return kill(pid_, SIGKILL) == 0 ? 0 : -errno;
}

bool Process::IsRunning()
{
    return pid_ > 0 && CollectExit(WNOHANG) == 0;
}

int Process::Wait()
{
    if (exited_)
    {
        // IsRunning may already have collected exit_status_.
        return 0;
    }
    if (pid_ <= 0)
    {
        return -ECHILD;
    }

    const int result = CollectExit(0);
    return result < 0 ? result : 0;
}

pid_t Process::GetPid() const
{
    return pid_;
}

int Process::CollectExit(int options)
{
    int status = 0;
    pid_t result;
    do
    {
        result = waitpid(pid_, &status, options);
    } while (result < 0 && errno == EINTR);

    if (result < 0)
    {
        const int error = errno;
        if (error == ECHILD)
        {
            // The child is no longer waitable; do not signal a stale PID.
            pid_ = -1;
            CloseSupervision();
        }
        return -error;
    }
    if (result == 0)
    {
        return 0;
    }

    exit_status_ = status;
    exited_ = true;
    pid_ = -1;
    CloseSupervision();
    return 1;
}

} // namespace kcf
