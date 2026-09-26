#include "mecanum/motor_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <charconv>
#include <iostream>
#include <string>

namespace {
bool PositiveNumber(const std::string& text, unsigned& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0;
}
void Usage() {
    std::cout << "Usage: kcf_mecanum_motor --serial DEVICE [--baud 115200]\n"
                 "       [--command-timeout-ms 250] [--topic-prefix /mecanum]\n";
}
}
int main(int argc, char** argv) {
    mecanum::MotorOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { Usage(); return 0; }
        if (++i >= argc) { Usage(); return 2; }
        const std::string value = argv[i];
        unsigned number = 0;
        if (key == "--serial") options.device = value;
        else if (key == "--topic-prefix") options.topic_prefix = value;
        else if (key == "--baud" && PositiveNumber(value, number) && number <= 4000000)
            options.baudrate = static_cast<int>(number);
        else if (key == "--command-timeout-ms" && PositiveNumber(value, number) && number <= 10000)
            options.command_timeout_ns = static_cast<std::uint64_t>(number) * 1000000ULL;
        else { std::cerr << "Invalid argument: " << key << '\n'; return 2; }
    }
    if (options.device.empty() || options.topic_prefix.size() < 2 ||
        options.topic_prefix.front() != '/' || options.topic_prefix.back() == '/') {
        Usage();
        return 2;
    }
    const auto mode = kcf::DetectLaunchExecutionMode();
    std::cout << "Motor mode=" << (mode == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised") << '\n';
    mecanum::MotorElement element(options);
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(1000.0 / 15.0);
    const int result = runtime.Run(element);
    if (result || element.ShutdownError()) {
        std::cerr << "Motor failed: runtime=" << result << " shutdown=" << element.ShutdownError() << '\n';
        return 1; // do not truncate negative errno into a possibly successful exit code
    }
    return 0;
}
