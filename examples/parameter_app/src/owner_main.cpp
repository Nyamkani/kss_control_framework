#include "parameter_app/example_config.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>

class ParameterOwnerElement : public kcf::ProcessElement
{
public:
    int Setup() override
    {
        const int result = parameter_.Create(parameter_app::PARAMETER_NAME,
            {true, 10, 1.0f, {-1.0f, 1.0f}},
            [this](const parameter_app::ExampleConfig&) {
                notified_.store(true, std::memory_order_relaxed);
            });
        owned_ = result == 0;
        return result;
    }

    int Loop() override
    {
        parameter_app::ExampleConfig current{};
        const int result = parameter_.Get(current);
        if (result) return result;
        if (notified_.exchange(false, std::memory_order_relaxed))
            std::cout << "[Parameter] watcher notification" << std::endl;
        // Compare fields, not POD padding. Get is the source of current state.
        if (!printed_ || current.enabled != last_.enabled || current.count != last_.count ||
            current.gain != last_.gain || current.limits[0] != last_.limits[0] ||
            current.limits[1] != last_.limits[1])
        {
            if (printed_) std::cout << "[Parameter] changed" << std::endl;
            std::cout << std::boolalpha << "[Parameter] enabled=" << current.enabled
                      << " count=" << current.count << " gain=" << current.gain
                      << " limits=[" << current.limits[0] << ", " << current.limits[1]
                      << "]" << std::endl;
            last_ = current;
            printed_ = true;
        }
        return 0;
    }

    void Shutdown() override
    {
        if (!owned_) return;
        const int closed = parameter_.Close(); // joins watcher before member destruction
        const int unlinked = parameter_.Unlink();
        owned_ = false;
        if (closed || unlinked) throw std::runtime_error("Parameter cleanup failed");
        std::cout << "[Parameter] Shutdown complete" << std::endl;
    }
private:
    std::atomic<bool> notified_{false};
    kcf::Parameter<parameter_app::ExampleConfig> parameter_;
    parameter_app::ExampleConfig last_{};
    bool owned_{false};
    bool printed_{false};
};

int main()
{
    const auto mode = kcf::DetectLaunchExecutionMode();
    std::cout << "[Parameter] mode="
              << (mode == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")
              << std::endl;
    ParameterOwnerElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(10.0);
    return runtime.Run(element);
}
