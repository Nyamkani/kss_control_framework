// Supervisor test only: intentional SIGSEGV or a selected normal exit code.
#include <charconv>
#include <chrono>
#include <iostream>
#include <signal.h>
#include <sys/prctl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include "kcf/process/process_runtime.hpp"

class CrashElement : public kcf::ProcessElement
{
public:
    unsigned delay{500}, exit_code{0};
    bool normal_exit{false};
    std::string name{"crash"};
    int Setup() override
    {
        start=std::chrono::steady_clock::now();
        std::cout << "[CrashTest] " << name << " delay_ms=" << delay << std::endl;
        return 0;
    }
    void Loop() override
    {
        if (std::chrono::steady_clock::now()-start<std::chrono::milliseconds(delay)) return;
        if (normal_exit) _exit(static_cast<int>(exit_code)); // intentional unexpected process exit
        if (prctl(PR_SET_DUMPABLE,0)!=0) _exit(2);
        raise(SIGSEGV);
    }
private:
    std::chrono::steady_clock::time_point start{};
};

int main(int argc, char** argv)
{
    CrashElement element;
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 >= argc) return 2;
        const std::string option = argv[i], value = argv[i + 1];
        if (option == "--element-name") { element.name = value; continue; }
        unsigned number = 0;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 2;
        if (option == "--crash-after-ms" && number <= 60000) element.delay = number;
        else if (option == "--exit-code" && number <= 255) { element.exit_code = number; element.normal_exit = true; }
        else return 2;
    }
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(100.0);
    return runtime.Run(element);
}
