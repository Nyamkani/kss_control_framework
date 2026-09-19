#include "bringup/bringup.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <cerrno>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>
#include <algorithm>
#include <cstring>
#include <limits>
#include <poll.h>

namespace
{
volatile sig_atomic_t stop_requested = 0;
void HandleStopSignal(int) { stop_requested = 1; }
const char* StateName(bringup::ApplicationState state)
{
    switch (state)
    {
    case bringup::ApplicationState::INITIALIZING: return "INITIALIZING";
    case bringup::ApplicationState::RUNNING: return "RUNNING";
    case bringup::ApplicationState::ERROR: return "ERROR";
    case bringup::ApplicationState::RESETTING: return "RESETTING";
    case bringup::ApplicationState::SHUTTING_DOWN: return "SHUTTING_DOWN";
    }
    return "UNKNOWN";
}
}
namespace bringup
{
int Bringup::Setup(const std::string& executable)
{
    return Setup(std::vector<ElementSpec>{{"element1", executable, {}}});
}
int Bringup::Setup(std::vector<ElementSpec> elements)
{
    return Setup("", std::move(elements));
}
int Bringup::Setup(const std::string& application_name, std::vector<ElementSpec> elements)
{
    // Setup is only for the initial lifecycle; Reset is processed by Run.
    if (application_state_ != ApplicationState::INITIALIZING ||
        setup_ || sigint_installed_ || sigterm_installed_) return -EBUSY;
    if (elements.empty()) return -EINVAL;
    // Allocate and validate before starting any child.
    elements_.clear();
    for (auto& spec : elements)
    {
        if (spec.name.empty() || spec.executable.empty() || !spec.startup_timeout_ms || !spec.health_timeout_ms || !spec.shutdown_timeout_ms) return -EINVAL;
        for (const auto& entry : elements_)
            if (entry->spec.name == spec.name) return -EINVAL;
        auto entry = std::make_unique<ManagedElement>();
        entry->spec = std::move(spec);
        elements_.push_back(std::move(entry));
    }
    stop_requested = 0;
    exit_code_ = 0;
    struct sigaction action {};
    action.sa_handler = HandleStopSignal;
    if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, &previous_sigint_)) return -errno;
    sigint_installed_ = true;
    if (sigaction(SIGTERM, &action, &previous_sigterm_))
    {
        const int error = errno;
        RestoreSignalHandlers();
        return -error;
    }
    sigterm_installed_ = true;
    kcf::SystemStatus initial{}; // current INITIALIZING exists before child exec
    initial.sequence = 1;
    const int created = status_publisher_.CreateForApplication(initial);
    if (created)
    {
        std::cerr << "[Supervisor] SystemStatus Create error=" << created << std::endl;
        RestoreSignalHandlers();
        return created;
    }
    status_owned_ = true;
    status_sequence_ = initial.sequence;
    // Display identity only. Failure to obtain a fallback never affects Setup.
    try
    {
        const auto name = application_name.empty() ? kcf::detail::CurrentExecutableBasename() : application_name;
        std::memset(application_name_, 0, sizeof(application_name_));
        std::memcpy(application_name_, name.data(), std::min(name.size(), sizeof(application_name_) - 1));
    }
    catch (...) {}
    introspection_.Begin();
    PublishIntrospection();
    int result = StartGeneration();
    if (!result) result = AwaitRunning();
    if (result || stop_requested)
    {
        exit_code_ = 1;
        Shutdown();
        return result ? result : -ECANCELED;
    }
    setup_ = true;
    SetApplicationState(ApplicationState::RUNNING);
    return 0;
}
int Bringup::StartGeneration()
{
    for (auto& entry : elements_)
    {
        if (stop_requested) return -ECANCELED;
        int result;
        try { result = entry->process.Start(entry->spec.executable, entry->spec.arguments, status_publisher_.Scope()); }
        catch (...) { result = -ENOMEM; }
        if (result)
        {
            RecordFailure(*entry, kcf::ElementFailureKind::RUNTIME_ERROR, result);
            std::cerr << "[Bringup] startup failed name=" << entry->spec.name << " error=" << result << std::endl;
            return result;
        }
        entry->started = entry->process_alive = true;
        entry->started_at = entry->last_response_time = entry->last_heartbeat_change_time = Clock::now();
        entry->pid = entry->process.GetPid();
        // An unavailable generation only limits metadata; child ownership is unchanged.
        try { (void)kcf::detail::ReadProcessIdentity(entry->pid, entry->process_start_ticks); }
        catch (...) { entry->process_start_ticks = 0; }
        std::cout << "[Bringup] started name=" << entry->spec.name << " pid=" << entry->pid << std::endl;
        PublishSystemStatus();
    }
    return 0;
}
int Bringup::AwaitRunning()
{
    for (;;)
    {
        const auto cycle = Clock::now();
        if (stop_requested) return -ECANCELED;
        int failure = PollRuntimeStatuses();
        bool ready = true;
        for (auto& entry : elements_)
        {
            int error = 0;
            auto kind = kcf::ElementFailureKind::RUNTIME_ERROR;
            if (!entry->process.IsRunning())
            { entry->process_alive = false; error = -ECHILD; kind = kcf::ElementFailureKind::PROCESS_EXIT; }
            else if (entry->status_valid &&
                     ((entry->last_runtime_status.state != kcf::ProcessState::STARTING &&
                       entry->last_runtime_status.state != kcf::ProcessState::RUNNING) ||
                      entry->last_runtime_status.runtime_error))
                error = entry->last_runtime_status.runtime_error ? entry->last_runtime_status.runtime_error : -EPROTO;
            else if (!entry->runtime_ready || entry->last_runtime_status.state != kcf::ProcessState::RUNNING)
            {
                ready = false;
                if (Clock::now() - entry->started_at >= std::chrono::milliseconds(entry->spec.startup_timeout_ms))
                {
                    error = -ETIMEDOUT;
                    kind = kcf::ElementFailureKind::STATUS_TIMEOUT;
                    std::cerr << "[Supervisor] startup timeout name=" << entry->spec.name << std::endl;
                }
            }
            else if (Clock::now() - entry->last_response_time >= std::chrono::milliseconds(entry->spec.health_timeout_ms))
            { error = -ETIMEDOUT; kind = kcf::ElementFailureKind::STATUS_TIMEOUT; }
            if (error) { RecordFailure(*entry, kind, error); failure = error; }
        }
        if (stop_requested) return -ECANCELED;
        if (failure)
        {
            std::cerr << "[Supervisor] startup barrier failed error=" << failure << std::endl;
            return failure;
        }
        if (ready) return 0;
        PublishSystemStatus();
        std::this_thread::sleep_until(cycle + std::chrono::milliseconds(100));
    }
}
void Bringup::RequestReset()
{
    unsigned open = 1;
    reset_gate_.compare_exchange_strong(open, 2);
}
void Bringup::ResetGeneration()
{
    SetApplicationState(ApplicationState::RESETTING);
    int result = StopGeneration();
    if (stop_requested) return;
    if (!result)
    {
        // Old Process objects retain no live PID/fd. Keep specs and replace only
        // per-generation observations; the first ApplicationError stays latched.
        for (auto& entry : elements_)
        {
            entry->pid = -1;
            entry->process_start_ticks = 0;
            entry->failure_detected = false;
            entry->exit_info = {};
            entry->failure_kind = kcf::ElementFailureKind::NONE;
            entry->runtime_error = 0;
            entry->status_valid = entry->runtime_ready = false;
            entry->last_runtime_status = {};
            entry->started_at = entry->last_response_time = entry->last_heartbeat_change_time = {};
        }
        PublishIntrospection();
        request_id_ = 0;
        result = StartGeneration();
        if (!result) result = AwaitRunning();
    }
    if (stop_requested) return;
    if (result)
    {
        StopGeneration();
        if (stop_requested) return;
        SetApplicationState(ApplicationState::ERROR);
        std::cerr << "[Supervisor] Reset failed error=" << result << std::endl;
        return;
    }
    application_error_ = {};
    exit_code_ = 0;
    SetApplicationState(ApplicationState::RUNNING);
    std::cout << "[Supervisor] Reset complete" << std::endl;
}
void Bringup::SetApplicationState(ApplicationState state)
{
    if (application_state_ == state) return;
    std::cout << "[Supervisor] " << StateName(application_state_) << " -> " << StateName(state) << std::endl;
    reset_gate_.store(state == ApplicationState::ERROR ? 1u : 0u);
    application_state_ = state;
    PublishSystemStatus();
}
void Bringup::PublishIntrospection() noexcept
{
    kcf::SupervisorInfo snapshot{};
    std::memcpy(snapshot.application_name, application_name_, sizeof(application_name_));
    snapshot.application_state = application_state_;
    snapshot.element_count = static_cast<std::uint32_t>(std::min(elements_.size(), kcf::MAX_SUPERVISOR_ELEMENTS));
    for (std::size_t i = 0; i < snapshot.element_count; ++i)
    {
        const auto& entry = *elements_[i];
        auto& child = snapshot.elements[i];
        std::memcpy(child.name, entry.spec.name.data(), std::min(entry.spec.name.size(), sizeof(child.name) - 1));
        std::memcpy(child.executable, entry.spec.executable.data(), std::min(entry.spec.executable.size(), sizeof(child.executable) - 1));
        child.pid = entry.pid;
        child.process_start_ticks = entry.process_start_ticks;
        child.process_alive = entry.process_alive ? 1 : 0;
    }
    introspection_.Update(snapshot);
}
void Bringup::PublishSystemStatus()
{
    PublishIntrospection();
    if (!status_owned_) return;
    int result = -EOVERFLOW;
    if (status_sequence_ != std::numeric_limits<std::uint64_t>::max())
    {
        kcf::SystemStatus snapshot{};
        snapshot.sequence = ++status_sequence_;
        snapshot.state = application_state_;
        if (application_error_.valid)
        {
            snapshot.error_valid = 1;
            snapshot.failure_kind = application_error_.failure_kind;
            snapshot.runtime_error = application_error_.runtime_error;
            snapshot.failed_pid = application_error_.pid;
            const auto& info = application_error_.exit_info;
            if (info.signaled) snapshot.termination_kind = kcf::ProcessTerminationKind::SIGNALED;
            else if (info.exited_normally) snapshot.termination_kind = kcf::ProcessTerminationKind::EXITED;
            snapshot.exit_code = info.exit_code;
            snapshot.signal_number = info.signal_number;
            const auto size = std::min(application_error_.element_name.size(), sizeof(snapshot.error_element) - 1);
            std::memcpy(snapshot.error_element, application_error_.element_name.data(), size);
        }
        result = status_publisher_.Publish(snapshot);
    }
    // One attempt only. Saturate sequence at UINT64_MAX instead of wrapping.
    // Deduplicate persistent IPC errors; there is no new internal-error FSM.
    if (result != 0 && result != last_publish_error_)
        std::cerr << "[Supervisor] SystemStatus Publish error=" << result << std::endl;
    last_publish_error_ = result;
}
std::vector<ManagedElementStatus> Bringup::GetElementStatuses() const
{
    std::vector<ManagedElementStatus> result;
    result.reserve(elements_.size());
    for (const auto& entry : elements_)
        result.push_back({entry->spec.name, entry->pid,
                          entry->process_alive,
                          entry->failure_detected, entry->exit_info, entry->failure_kind, entry->runtime_error});
    return result;
}
void Bringup::RecordFailure(ManagedElement& element, kcf::ElementFailureKind kind, int runtime_error)
{
    if (element.failure_detected) return;
    element.failure_detected = true;
    element.exit_info = kind==kcf::ElementFailureKind::PROCESS_EXIT ? element.process.GetExitInfo() : kcf::ProcessExitInfo{};
    element.failure_kind = kind;
    element.runtime_error = runtime_error;
    exit_code_ = 1;
    if (application_state_ == ApplicationState::RUNNING && !application_error_.valid)
    {
        application_error_ = {true, element.spec.name, element.pid, element.exit_info, kind, runtime_error};
        SetApplicationState(ApplicationState::ERROR);
        const auto& origin = application_error_;
        std::cerr << "[Supervisor] Origin name=" << origin.element_name << " pid=" << origin.pid
                  << " exited_normally=" << origin.exit_info.exited_normally
                  << " exit_code=" << origin.exit_info.exit_code
                  << " signaled=" << origin.exit_info.signaled
                  << " signal=" << origin.exit_info.signal_number
                  << " failure_kind=" << int(origin.failure_kind) << " runtime_error=" << origin.runtime_error << std::endl;
    }
    const auto& info = element.exit_info;
    std::cerr << "[Bringup] unexpected exit name=" << element.spec.name << " pid=" << element.pid
              << " available=" << info.available << " exited_normally=" << info.exited_normally
              << " exit_code=" << info.exit_code << " signaled=" << info.signaled
              << " signal=" << info.signal_number << " failure_kind=" << int(kind) << " runtime_error=" << runtime_error << std::endl;
}
int Bringup::PollRuntimeStatuses()
{
    if (request_id_ == std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
    ++request_id_;
    std::vector<pollfd> fds;
    fds.reserve(elements_.size());
    for (auto& entry : elements_)
    {
        const int fd = entry->process.GetSupervisionFd();
        if (!entry->process_alive || fd<0) continue;
        // Nonblocking send; failed/late responses never refresh health timers.
        const int sent=entry->process.RequestRuntimeStatus(request_id_);
        if (sent==0 || sent==-EBUSY) fds.push_back({fd,POLLIN,0});
    }
    // One shared poll budget, never N * per-child timeout. Drain every child
    // nonblocking as additional responses may arrive during this scan.
    if (!fds.empty()) poll(fds.data(), fds.size(), 20);
    for (auto& entry : elements_)
    {
        if (!entry->process_alive) continue;
        for (int packet=0; packet<8; ++packet)
        {
            kcf::RuntimeStatusResponse response{};
            const int result=entry->process.ReceiveRuntimeStatus(response);
            if (result==-EAGAIN || result==-EWOULDBLOCK || result==-ECONNRESET || result==-EBADF) break;
            if (result) continue;
            const auto now=Clock::now();
            if (!entry->status_valid || response.loop_heartbeat!=entry->last_runtime_status.loop_heartbeat)
                entry->last_heartbeat_change_time=now;
            if (!entry->status_valid || response.state!=entry->last_runtime_status.state)
                std::cout << "[Supervisor] runtime name=" << entry->spec.name << " state=" << int(response.state)
                          << " heartbeat=" << response.loop_heartbeat << " error=" << response.runtime_error << std::endl;
            entry->last_runtime_status=response;
            entry->status_valid=true;
            entry->last_response_time=now;
            if (response.state==kcf::ProcessState::RUNNING && !entry->runtime_ready)
            { entry->runtime_ready=true; entry->last_heartbeat_change_time=now; }
        }
    }
    return 0;
}
int Bringup::Run()
{
    if (!setup_) return -EINVAL;
    running_=true;
    while (running_ && !stop_requested)
    {
        unsigned pending = 2;
        if (reset_gate_.compare_exchange_strong(pending, 0))
        {
            ResetGeneration();
            if (stop_requested) break;
        }
        const auto cycle=Clock::now();
        const int polling=PollRuntimeStatuses();
        for (auto& entry : elements_)
        {
            if (stop_requested) break;
            if (!entry->process_alive) continue;
            if (!entry->process.IsRunning())
            {
                entry->process_alive=false;
                if (!entry->failure_detected) RecordFailure(*entry);
                else entry->exit_info=entry->process.GetExitInfo();
                continue;
            }
            if (entry->failure_detected) continue;
            const auto timeout=std::chrono::milliseconds(entry->spec.health_timeout_ms);
            const auto& status=entry->last_runtime_status;
            if (polling==-EOVERFLOW)
                RecordFailure(*entry,kcf::ElementFailureKind::RUNTIME_ERROR,polling);
            else if (Clock::now()-entry->last_response_time>=timeout)
                RecordFailure(*entry,kcf::ElementFailureKind::STATUS_TIMEOUT);
            else if (status.state!=kcf::ProcessState::RUNNING || status.runtime_error)
                RecordFailure(*entry,kcf::ElementFailureKind::RUNTIME_ERROR,
                              status.runtime_error ? status.runtime_error : -EPROTO);
            else if (Clock::now()-entry->last_heartbeat_change_time>=timeout)
                RecordFailure(*entry,kcf::ElementFailureKind::HEARTBEAT_STALL);
        }
        if (!stop_requested)
        {
            PublishSystemStatus();
            std::this_thread::sleep_until(cycle+std::chrono::milliseconds(100));
        }
    }
    Shutdown();
    return exit_code_;
}
int Bringup::StopGeneration()
{
    const auto start = Clock::now();
    int failure = 0;
    std::vector<bool> forced(elements_.size(), false);
    for (auto& entry : elements_)
    {
        if (!entry->started || !entry->process.IsRunning()) continue;
        const int result = entry->process.RequestStop();
        if (result && result != -ESRCH) failure = result;
        std::cout << "[Bringup] stop name=" << entry->spec.name << " pid=" << entry->pid
                  << " error=" << result << std::endl;
    }
    for (;;)
    {
        const auto cycle = Clock::now();
        if (stop_requested) SetApplicationState(ApplicationState::SHUTTING_DOWN);
        bool remaining = false;
        for (std::size_t i = 0; i < elements_.size(); ++i)
        {
            auto& entry = elements_[i];
            if (!entry->started) continue;
            if (!entry->process.IsRunning())
            {
                const auto info = entry->process.GetExitInfo();
                if (!info.available) failure = -ECHILD;
                entry->exit_info = info;
                entry->started = entry->process_alive = false;
                std::cout << "[Bringup] reaped name=" << entry->spec.name << " pid=" << entry->pid
                          << " signal=" << info.signal_number << std::endl;
                continue;
            }
            remaining = true;
            if (!forced[i] && cycle - start >= std::chrono::milliseconds(entry->spec.shutdown_timeout_ms))
            {
                const int result = entry->process.ForceStop();
                if (result && result != -ESRCH) return result; // never start a new generation
                forced[i] = true;
                std::cout << "[Bringup] force stop name=" << entry->spec.name << " pid=" << entry->pid
                          << " error=" << result << std::endl;
            }
        }
        PublishIntrospection();
        if (!remaining) return failure;
        PublishSystemStatus();
        std::this_thread::sleep_until(cycle + std::chrono::milliseconds(100));
    }
}
void Bringup::Shutdown()
{
    SetApplicationState(ApplicationState::SHUTTING_DOWN);
    if (StopGeneration()) exit_code_ = 1;
    PublishIntrospection();
    introspection_.End();
    if (status_owned_)
    {
        const int closed = status_publisher_.Close();
        const int unlinked = status_publisher_.Unlink();
        if (closed || unlinked)
        {
            exit_code_ = 1;
            std::cerr << "[Supervisor] SystemStatus cleanup close=" << closed << " unlink=" << unlinked << std::endl;
        }
        status_owned_ = false;
    }
    running_ = false;
    setup_ = false;
    RestoreSignalHandlers();
    std::cout << "[Bringup] Shutdown complete" << std::endl;
}
void Bringup::RestoreSignalHandlers()
{
    if (sigterm_installed_) { sigaction(SIGTERM, &previous_sigterm_, nullptr); sigterm_installed_ = false; }
    if (sigint_installed_) { sigaction(SIGINT, &previous_sigint_, nullptr); sigint_installed_ = false; }
}
} // namespace bringup
