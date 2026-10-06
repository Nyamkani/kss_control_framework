#include "kcf/parameter/parameter.hpp"
#include "parameter/test_parameter.hpp"
#include <charconv>
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
    unsigned delay_ms = 0;
    if (argc > 2) return 1;
    if (argc == 2)
    {
        const std::string_view input(argv[1]);
        const auto result = std::from_chars(input.data(), input.data() + input.size(), delay_ms);
        if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || delay_ms > 5000)
        { std::cerr << "Usage: " << argv[0] << " [callback_delay_ms: 0..5000]" << std::endl; return 1; }
    }
    struct sigaction action {};
    action.sa_handler = HandleStop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) return 1;
    kcf::Parameter<ControlParameter> parameter;
    const auto main_thread = std::this_thread::get_id();
    int result = parameter.Create("/kcf_test_parameter", {1.0, 0.1, 0.01, 1}, [=](const ControlParameter& value)
    {
        std::cout << "[ParameterOwner] changed kp=" << value.kp << " ki=" << value.ki
                  << " kd=" << value.kd << " mode=" << value.mode
                  << " worker=" << (std::this_thread::get_id() != main_thread) << std::endl;
        if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });
    if (result != 0) { std::cerr << "Create failed: " << result << std::endl; return 1; }
    std::cout << "[ParameterOwner] ready initial=1,0.1,0.01,1 version=0" << std::endl;
    while (!stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int closed = parameter.Close();
    const int unlinked = parameter.Unlink();
    if (closed != 0 || unlinked != 0) return 1;
    std::cout << "[ParameterOwner] cleanup complete" << std::endl;
    return 0;
}
