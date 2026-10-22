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
    case bringup::ApplicationState::STOPPED: return "STOPPED";
    }
    return "UNKNOWN";
}
}
namespace bringup
{
int Bringup::ValidateElements(const std::vector<ElementSpec>& elements,
                              RegistrationError* error)
{
    if (error) *error = {};
    const auto reject = [&](std::optional<std::size_t> index, const char* field,
                            const char* reason) {
        if (error)
        {
            error->element_index = index;
            if (index) error->element_name = elements[*index].name;
            error->field = field;
            error->reason = reason;
        }
        return -EINVAL;
    };
    if (elements.empty()) return reject(std::nullopt, "elements", "empty");
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        const auto& spec = elements[i];
        if (spec.name.empty()) return reject(i, "name", "empty");
        if (spec.executable.empty()) return reject(i, "executable", "empty");
        if (!spec.startup_timeout_ms) return reject(i, "startup_timeout_ms", "zero");
        if (!spec.health_timeout_ms) return reject(i, "health_timeout_ms", "zero");
        if (!spec.shutdown_timeout_ms) return reject(i, "shutdown_timeout_ms", "zero");
        for (std::size_t j = 0; j < i; ++j)
            if (elements[j].name == spec.name) return reject(i, "name", "duplicate");
    }
    return 0;
}
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
    const int validation = ValidateElements(elements);
    if (validation) return validation;
    // Validation finished before changing registration or acquiring resources.
    elements_.clear();
    for (auto& spec : elements)
    {
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
    if (ShutdownRequested())
    {
        Shutdown();
        return -ECANCELED;
    }
    setup_ = true;
    SetApplicationState(result ? ApplicationState::ERROR : ApplicationState::RUNNING);
    return 0;
}
int Bringup::StartGeneration()
{
    ++generation_;
    for (auto& entry : elements_)
    {
        if (ShutdownRequested()) return -ECANCELED;
        int result;
        try { result = entry->process.Start(entry->spec.executable, entry->spec.arguments, status_publisher_.Scope(), entry->spec.execution_profile); }
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
        if (ShutdownRequested()) return -ECANCELED;
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
            if (error) { RecordFailure(*entry, kind, kind==kcf::ElementFailureKind::PROCESS_EXIT ? 0 : error); failure = error; }
        }
        if (ShutdownRequested()) return -ECANCELED;
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
bool Bringup::ShutdownRequested() const
{
    return stop_requested || shutdown_requested_.load();
}
void Bringup::RequestShutdown()
{
    shutdown_requested_.store(true);
}
void Bringup::RequestReset()
{
    if (ShutdownRequested()) return;
    unsigned open = 1;
    reset_gate_.compare_exchange_strong(open, 2);
}
void Bringup::ResetGeneration()
{
    SetApplicationState(ApplicationState::RESETTING);
    int result = StopGeneration();
    if (ShutdownRequested()) return;
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
    if (ShutdownRequested()) return;
    if (result)
    {
        StopGeneration();
        if (ShutdownRequested()) return;
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
    if (element.failure_detected || ShutdownRequested()) return;
    element.failure_detected = true;
    element.exit_info = kind==kcf::ElementFailureKind::PROCESS_EXIT ? element.process.GetExitInfo() : kcf::ProcessExitInfo{};
    element.failure_kind = kind;
    element.runtime_error = runtime_error;
    exit_code_ = 1;
    const bool observed = element.status_valid && element.last_runtime_status.runtime_error != 0;
    if (kind==kcf::ElementFailureKind::PROCESS_EXIT && observed)
        element.runtime_error=element.last_runtime_status.runtime_error;
    ApplicationError event{true,element.spec.name,element.pid,element.exit_info,kind,element.runtime_error};
    if (error_history_.size()==ERROR_HISTORY_CAPACITY) error_history_.pop_front();
    error_history_.push_back({++error_sequence_,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count()),generation_,event,observed});
    if (!application_error_.valid)
    {
        application_error_ = event;
        if (application_state_ != ApplicationState::RESETTING)
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
void Bringup::UpdateExitInfo(ManagedElement& element)
{
    element.exit_info=element.process.GetExitInfo();
    if (!element.failure_detected) return;
    for (auto& event:error_history_)
        if (event.generation==generation_ && event.error.element_name==element.spec.name &&
            event.error.pid==element.pid) event.error.exit_info=element.exit_info;
    if (application_error_.valid && application_error_.pid==element.pid &&
        application_error_.element_name==element.spec.name)
        application_error_.exit_info=element.exit_info;
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
    while (running_ && !ShutdownRequested())
    {
        unsigned pending = 2;
        if (reset_gate_.compare_exchange_strong(pending, 0))
        {
            ResetGeneration();
            if (ShutdownRequested()) break;
        }
        const auto cycle=Clock::now();
        const int polling=PollRuntimeStatuses();
        for (auto& entry : elements_)
        {
            if (ShutdownRequested()) break;
            if (!entry->process_alive) continue;
            if (!entry->process.IsRunning())
            {
                entry->process_alive=false;
                if (!entry->failure_detected) RecordFailure(*entry);
                else UpdateExitInfo(*entry);
                continue;
            }
            if (entry->failure_detected) continue;
            const auto timeout=std::chrono::milliseconds(entry->spec.health_timeout_ms);
            const auto& status=entry->last_runtime_status;
            if (polling==-EOVERFLOW)
                RecordFailure(*entry,kcf::ElementFailureKind::RUNTIME_ERROR,polling);
            else if (Clock::now()-entry->last_response_time>=timeout)
                RecordFailure(*entry,kcf::ElementFailureKind::STATUS_TIMEOUT);
            else if (!entry->runtime_ready && !status.runtime_error &&
                     (!entry->status_valid || status.state==kcf::ProcessState::STARTING))
            {
                if (Clock::now()-entry->started_at>=std::chrono::milliseconds(entry->spec.startup_timeout_ms))
                    RecordFailure(*entry,kcf::ElementFailureKind::STATUS_TIMEOUT);
            }
            else if (status.state!=kcf::ProcessState::RUNNING || status.runtime_error)
                RecordFailure(*entry,kcf::ElementFailureKind::RUNTIME_ERROR,
                              status.runtime_error ? status.runtime_error : -EPROTO);
            else if (Clock::now()-entry->last_heartbeat_change_time>=timeout)
                RecordFailure(*entry,kcf::ElementFailureKind::HEARTBEAT_STALL);
        }
        if (!ShutdownRequested())
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
    // One force attempt per child per invocation, plus a bounded reap grace.
    // A successful SIGKILL does not guarantee prompt exit (e.g. kernel D-state).
    auto deadline = start + std::chrono::seconds(1);
    for (const auto& entry : elements_)
        if (entry->started) deadline = std::max(deadline,
            start + std::chrono::milliseconds(entry->spec.shutdown_timeout_ms) +
            std::chrono::seconds(1));
    for (auto& entry : elements_)
    {
        if (!entry->started || !entry->process.IsRunning()) continue;
        const int result = entry->process.RequestStop();
        if (result && result != -ESRCH && !failure) failure = result;
        std::cout << "[Bringup] stop name=" << entry->spec.name << " pid=" << entry->pid
                  << " error=" << result << std::endl;
    }
    for (;;)
    {
        const auto cycle = Clock::now();
        if (ShutdownRequested()) SetApplicationState(ApplicationState::SHUTTING_DOWN);
        bool remaining = false;
        for (std::size_t i = 0; i < elements_.size(); ++i)
        {
            auto& entry = elements_[i];
            if (!entry->started) continue;
            const bool alive = entry->process.IsRunning();
            const auto info = entry->process.GetExitInfo();
            if (!alive && info.available)
            {
                UpdateExitInfo(*entry);
                entry->started = entry->process_alive = false;
                std::cout << "[Bringup] reaped name=" << entry->spec.name << " pid=" << entry->pid
                          << " signal=" << info.signal_number << std::endl;
                continue;
            }
            // IsRunning=false can also mean waitpid failed. Do not treat that
            // as reaping, clear ownership, or signal a PID discarded by Process.
            remaining = true;
            if (!alive && !failure) failure = -ECHILD;
            if (entry->process.GetPid() <= 0) continue;
            if (!forced[i] && cycle - start >= std::chrono::milliseconds(entry->spec.shutdown_timeout_ms))
            {
                const int result = entry->process.ForceStop();
                if (result && result != -ESRCH && !failure) failure = result;
                forced[i] = true;
                std::cout << "[Bringup] force stop name=" << entry->spec.name << " pid=" << entry->pid
                          << " error=" << result << std::endl;
            }
        }
        PublishIntrospection();
        if (!remaining) return failure;
        if (Clock::now() >= deadline)
        {
            for (const auto& entry : elements_)
                if (entry->started)
                    std::cerr << "[Supervisor] unreaped child name=" << entry->spec.name
                              << " pid=" << entry->pid << " process_pid=" << entry->process.GetPid()
                              << " supervision_fd=" << entry->process.GetSupervisionFd() << std::endl;
            return failure ? failure : -ETIMEDOUT;
        }
        PublishSystemStatus();
        std::this_thread::sleep_until(cycle + std::chrono::milliseconds(100));
    }
}
void Bringup::Shutdown()
{
    if (application_state_==ApplicationState::STOPPED && !status_owned_ &&
        !sigint_installed_ && !sigterm_installed_) return;
    RequestShutdown();
    if (application_state_!=ApplicationState::STOPPED)
        SetApplicationState(ApplicationState::SHUTTING_DOWN);
    const int stopped = StopGeneration();
    if (stopped)
    {
        if (!exit_code_) exit_code_ = 1;
        std::cerr << "[Supervisor] StopGeneration error=" << stopped << std::endl;
    }
    const bool reaped = std::none_of(elements_.begin(), elements_.end(),
                                    [](const auto& entry) { return entry->started; });
    // A stop error alone does not imply live children; conversely an unknown
    // wait result is never proof of completion. Keep Process ownership for retry.
    if (reaped) SetApplicationState(ApplicationState::STOPPED);
    introspection_.End();
    if (status_owned_)
    {
        const int closed = status_publisher_.Close();
        const int unlinked = status_publisher_.Unlink();
        if (closed || unlinked)
        {
            if (!exit_code_) exit_code_ = 1;
            std::cerr << "[Supervisor] SystemStatus cleanup close=" << closed << " unlink=" << unlinked << std::endl;
        }
        // Close consumes the wrapper handles even on failure; retry only a
        // name still pending unlink, not an already unlinked object.
        status_owned_ = unlinked != 0 && unlinked != -ENOENT;
    }
    running_ = false;
    setup_ = false;
    RestoreSignalHandlers();
    std::cout << "[Bringup] Shutdown " << (reaped ? "complete" : "incomplete: child ownership retained") << std::endl;
}
void Bringup::RestoreSignalHandlers()
{
    const auto restore = [&](int signal, const struct sigaction& previous, bool& installed) {
        if (!installed) return;
        if (sigaction(signal, &previous, nullptr)==0) installed=false;
        else {
            if (!exit_code_) exit_code_=1;
            std::cerr << "[Supervisor] signal restore signal=" << signal << " error=" << -errno << std::endl;
        }
    };
    restore(SIGTERM,previous_sigterm_,sigterm_installed_);
    restore(SIGINT,previous_sigint_,sigint_installed_);
}
} // namespace bringup
