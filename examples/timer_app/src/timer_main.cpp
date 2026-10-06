#include "kcf/timer/timer.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>

class TimerElement : public kcf::ProcessElement
{
public:
    int Setup() override
    {
        return timer_.Create(2.0, [this] {
            tick_count_.fetch_add(1, std::memory_order_relaxed);
        });
    }
    int Loop() override
    {
        const auto tick = tick_count_.load(std::memory_order_relaxed);
        if (tick != observed_)
        {
            observed_ = tick;
            std::cout << "[Timer] tick=" << tick << std::endl;
        }
        return 0;
    }
    void Shutdown() override
    {
        timer_.Stop(); // joins any in-flight callback before returning
        std::cout << "[Timer] Shutdown complete ticks="
                  << tick_count_.load(std::memory_order_relaxed) << std::endl;
    }
private:
    // Timer destruction must precede destruction of callback state.
    std::atomic<std::uint64_t> tick_count_{0};
    kcf::Timer timer_;
    std::uint64_t observed_{0};
};

int main()
{
    const auto mode = kcf::DetectLaunchExecutionMode();
    std::cout << "[Timer] mode="
              << (mode == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")
              << std::endl;
    TimerElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(20.0);
    return runtime.Run(element);
}
