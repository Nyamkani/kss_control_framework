#include "service_app/add_service.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <iostream>
#include <limits>

class ServiceServerElement : public kcf::ProcessElement
{
public:
    int Setup() override
    {
        int result = server_.Create(service_app::PORT);
        if (result) return result;
        result = server_.Register<service_app::AddRequest, service_app::AddResponse>(
            service_app::ADD_ID, service_app::SERVICE_NAME,
            [](const auto& request, auto& response) {
                const std::int64_t sum = static_cast<std::int64_t>(request.a) + request.b;
                response.accepted = sum >= std::numeric_limits<std::int32_t>::min() &&
                                    sum <= std::numeric_limits<std::int32_t>::max();
                response.result = response.accepted ? static_cast<std::int32_t>(sum) : 0;
                std::cout << "[Service] request a=" << request.a << " b=" << request.b << '\n'
                          << "[Service] response result=" << response.result
                          << " accepted=" << std::boolalpha << response.accepted << std::endl;
            });
        return result ? result : server_.Start();
    }
    int Loop() override { return 0; }
    void Shutdown() override
    {
        server_.Stop();
        std::cout << "[Service] Shutdown complete" << std::endl;
    }
private:
    kcf::ServiceServer server_;
};
int main()
{
    const auto mode = kcf::DetectLaunchExecutionMode();
    std::cout << "[Service] mode="
              << (mode == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")
              << std::endl;
    ServiceServerElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(10.0);
    return runtime.Run(element);
}
