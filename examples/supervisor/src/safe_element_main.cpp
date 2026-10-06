#include "kcf/process/process_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/system/system_status.hpp"
#include "kcf/system/system_status_channel.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>

class SafeElement : public kcf::ProcessElement
{
public:
    int Setup() override
    {
        const int result = status_sub_.Open( [this](const kcf::SystemStatus& status)
        {
            operational_.store(status.state == kcf::ApplicationState::RUNNING);
            if (status.state == kcf::ApplicationState::ERROR) error_requested_.store(true);
        });
        if (result) std::cerr << "[SafeElement] Setup failed: SystemStatus unavailable error=" << result << std::endl;
        else std::cout << "[SafeElement] Setup output=0" << std::endl;
        return result;
    }
    int Loop() override
    {
        if (error_requested_.load() && !safe_)
        {
            safe_ = true;
            output_ = 0; // simulated output, changed only in application context
            std::cout << "[SafeElement] Enter SAFE state output=0" << std::endl;
        }
        output_ = !safe_ && operational_.load() ? 100 : 0;
        // Low-rate diagnostics demonstrate that the process/FSM remains alive.
        if (++ticks_ % 10 == 0)
            std::cout << "[SafeElement] alive safe=" << safe_ << " output=" << output_ << std::endl;
        return 0;
    }
    void Shutdown() override
    {
        output_ = 0;
        const int result = status_sub_.Close();
        std::cout << "[SafeElement] Shutdown safe=" << safe_ << " output=" << output_ << " error=" << result << std::endl;
        if (result) throw std::runtime_error("SystemStatus Subscriber Close failed");
    }
private:
    std::atomic<bool> operational_{false};
    std::atomic<bool> error_requested_{false};
    kcf::SystemStatusSubscriber status_sub_;
    bool safe_{false};
    int output_{0};
    unsigned ticks_{0};
};
int main()
{
    SafeElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(20.0);
    return runtime.Run(element);
}
