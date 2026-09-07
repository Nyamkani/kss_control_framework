#pragma once

#include <atomic>
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

    std::atomic<bool> running_{false};
    std::atomic<ProcessState> state_{ProcessState::STOPPED};
    double loop_frequency_hz_{10.0};

    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
    bool sigint_installed_{false};
    bool sigterm_installed_{false};
};

} // namespace kcf
