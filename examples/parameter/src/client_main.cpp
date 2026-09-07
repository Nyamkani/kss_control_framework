#include "kcf/parameter/parameter.hpp"
#include "parameter/test_parameter.hpp"
#include <chrono>
#include <iostream>
#include <string_view>
#include <signal.h>

namespace
{
volatile sig_atomic_t stop_requested = 0;
void HandleStop(int) { stop_requested = 1; }
}

int main(int argc, char* argv[])
{
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "watch"))
    { std::cerr << "Usage: " << argv[0] << " [watch]" << std::endl; return 1; }
    struct sigaction action {};
    action.sa_handler = HandleStop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) return 1;
    kcf::Parameter<ControlParameter> parameter;
    std::function<void(const ControlParameter&)> callback;
    if (argc == 2) callback = [](const ControlParameter& value)
    {
        std::cout << "[ParameterClient] changed kp=" << value.kp << " ki=" << value.ki
                  << " kd=" << value.kd << " mode=" << value.mode << std::endl;
    };
    int result = parameter.Open("/kcf_test_parameter", callback);
    if (result != 0) { std::cerr << "Open failed: " << result << std::endl; return 1; }
    ControlParameter value{};
    std::uint64_t version = 0;
    if (parameter.Get(value, &version) != 0) return 1;
    std::cout << "[ParameterClient] current kp=" << value.kp << " ki=" << value.ki
              << " kd=" << value.kd << " mode=" << value.mode << " version=" << version << std::endl;
    if (argc == 2)
    {
        while (!stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    else
    {
        if (parameter.Set({2.0, 0.2, 0.02, 2}) != 0 || parameter.Get(value, &version) != 0) return 1;
        std::cout << "[ParameterClient] after Set kp=" << value.kp << " ki=" << value.ki
                  << " kd=" << value.kd << " mode=" << value.mode << " version=" << version << std::endl;
    }
    return parameter.Close() == 0 ? 0 : 1;
}
