#pragma once

#include <atomic>
#include <thread>
#include "kcf/process/runtime_supervision.hpp"
#include <signal.h>

#include "kcf/process/lifecycle.hpp"
#include "kcf/process/process_element.hpp"

namespace kcf
{

class ProcessRuntime
{
public:
    ProcessRuntime() = default;
    ~ProcessRuntime() = default;

    int Run(ProcessElement& element);
    void RequestStop();
    bool IsRunning() const;
    ProcessState GetState() const;
    void SetLoopFrequency(double hz);

private:
    int Initialize();
    void Finalize();
    int StartSupervision();
    void Supervise();
    void SupervisorLost(int error);

    // 0: no stop, 1: expected stop, negative errno: latched Supervisor loss.
    std::atomic<int> stop_reason_{0};
    std::atomic<bool> running_{false};
    std::atomic<ProcessState> state_{ProcessState::STOPPED};
    double loop_frequency_hz_{10.0};
    std::atomic<std::uint64_t> loop_heartbeat_{0};
    std::atomic<int> runtime_error_{0};
    std::atomic<bool> supervision_running_{false};
    std::thread supervision_worker_;
    int supervision_fd_{-1};

    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
    bool sigint_installed_{false};
    bool sigterm_installed_{false};
};

} // namespace kcf
