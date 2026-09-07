#include "kcf/process/process_runtime.hpp"

#include <chrono>
#include <cmath>
#include <thread>

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

    if (Initialize() != 0)
    {
        running_ = false;
        state_ = ProcessState::ERROR;
        Finalize();
        return -1;
    }

    int setup_result;
    try
    {
        setup_result = element.Setup();
    }
    catch (...)
    {
        setup_result = -1;
    }

    if (setup_result != 0)
    {
        running_ = false;
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
        result = -1;
        running_ = false;
    }

    state_ = ProcessState::STOPPING;
    try
    {
        element.Shutdown();
    }
    catch (...)
    {
        result = -1;
    }

    Finalize();
    running_ = false;
    state_ = result == 0 ? ProcessState::STOPPED : ProcessState::ERROR;
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
        return -1;
    }

    if (sigaction(SIGINT, &action, &previous_sigint_) != 0)
    {
        return -1;
    }
    sigint_installed_ = true;

    if (sigaction(SIGTERM, &action, &previous_sigterm_) != 0)
    {
        return -1;
    }
    sigterm_installed_ = true;
    return 0;
}

void ProcessRuntime::Finalize()
{
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
