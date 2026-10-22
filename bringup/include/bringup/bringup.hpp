#pragma once
#include <signal.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>
#include <deque>
#include <optional>
#include "kcf/process/process.hpp"
#include "kcf/introspection/detail/supervisor_registry.hpp"
#include "kcf/system/system_status.hpp"
#include "kcf/system/system_status_channel.hpp"

namespace bringup
{
using ApplicationState = kcf::ApplicationState; // source compatibility; one common type
struct ApplicationError
{
    bool valid{false};
    std::string element_name;
    pid_t pid{-1};
    kcf::ProcessExitInfo exit_info{};
    kcf::ElementFailureKind failure_kind{kcf::ElementFailureKind::NONE};
    int runtime_error{0};
};
struct ErrorHistoryEntry
{
    std::uint64_t sequence{0}, monotonic_ns{0}, generation{0};
    ApplicationError error;
    bool runtime_error_observed{false};
};
struct ManagedElementStatus
{
    std::string name;
    pid_t pid{-1};
    bool running{false};
    bool failure_detected{false};
    kcf::ProcessExitInfo exit_info{};
    kcf::ElementFailureKind failure_kind{kcf::ElementFailureKind::NONE};
    int runtime_error{0};
};
struct ElementSpec
{
    std::string name;
    std::string executable;
    std::vector<std::string> arguments;
    std::uint32_t startup_timeout_ms{5000};
    std::uint32_t health_timeout_ms{2000};
    std::uint32_t shutdown_timeout_ms{1500};
    kcf::ExecutionProfile execution_profile{};
};
struct RegistrationError
{
    // Zero-based index; absent for an empty list and after successful validation.
    std::optional<std::size_t> element_index;
    std::string element_name;
    std::string field;
    std::string reason;
};
class Bringup
{
public:
    // Pure structural validation. First error returns -EINVAL; optional output
    // is cleared on every call. Does not inspect files, privileges or runtime state.
    static int ValidateElements(const std::vector<ElementSpec>& elements,
                                RegistrationError* error = nullptr);
    // 0: Supervisor is ready for Run (RUNNING or child-failure ERROR).
    // Nonzero: Supervisor preparation failed, or startup was explicitly canceled.
    int Setup(std::vector<ElementSpec> elements);
    int Setup(const std::string& application_name, std::vector<ElementSpec> elements);
    int Setup(const std::string& executable);
    int Run();
    void Shutdown();
    void RequestShutdown(); // thread-safe request; not a signal-handler API
    void RequestReset(); // thread-safe request only; not a signal-handler API
    // Lifecycle and queries belong to the same controlling thread. Status is
    // the last monitor observation; queries do not poll/reap child processes.
    ApplicationState GetApplicationState() const { return application_state_; }
    const ApplicationError& GetApplicationError() const { return application_error_; }
    std::vector<ManagedElementStatus> GetElementStatuses() const;
    static constexpr std::size_t ERROR_HISTORY_CAPACITY = 64;
    // Oldest first, KEEP_LAST; same controlling-thread query contract.
    const std::deque<ErrorHistoryEntry>& GetErrorHistory() const { return error_history_; }

private:
    using Clock = std::chrono::steady_clock;
    struct ManagedElement
    {
        ElementSpec spec;
        kcf::Process process;
        pid_t pid{-1}; // preserved for exit diagnostics after Process reaps
        std::uint64_t process_start_ticks{0};
        bool started{false};
        bool failure_detected{false};
        kcf::ProcessExitInfo exit_info{};
        kcf::ElementFailureKind failure_kind{kcf::ElementFailureKind::NONE};
        int runtime_error{0};
        bool process_alive{false}, status_valid{false}, runtime_ready{false};
        kcf::RuntimeStatusResponse last_runtime_status{};
        Clock::time_point started_at{}, last_response_time{}, last_heartbeat_change_time{};
    };
    int StartGeneration();
    int AwaitRunning();
    int StopGeneration();
    void ResetGeneration();
    // 0: closed, 1: ERROR accepts a request, 2: one pending request.
    std::atomic<unsigned> reset_gate_{0};
    std::atomic<bool> shutdown_requested_{false};
    bool ShutdownRequested() const;
    void UpdateExitInfo(ManagedElement& element);
    void PublishSystemStatus();
    void PublishIntrospection() noexcept;
    char application_name_[128]{"supervisor"};
    kcf::detail::SupervisorRegistry introspection_;
    void SetApplicationState(ApplicationState state);
    int PollRuntimeStatuses();
    void RecordFailure(ManagedElement& element,
                       kcf::ElementFailureKind kind = kcf::ElementFailureKind::PROCESS_EXIT,
                       int runtime_error = 0);
    void RestoreSignalHandlers();
    std::vector<std::unique_ptr<ManagedElement>> elements_;
    bool running_{false}, setup_{false};
    int exit_code_{0};
    ApplicationState application_state_{ApplicationState::INITIALIZING};
    ApplicationError application_error_{};
    std::deque<ErrorHistoryEntry> error_history_;
    std::uint64_t generation_{0}, error_sequence_{0};
    kcf::SystemStatusPublisher status_publisher_;
    bool status_owned_{false};
    std::uint64_t status_sequence_{0};
    int last_publish_error_{0};
    std::uint64_t request_id_{0};
    struct sigaction previous_sigint_ {}, previous_sigterm_ {};
    bool sigint_installed_{false}, sigterm_installed_{false};
};
} // namespace bringup
