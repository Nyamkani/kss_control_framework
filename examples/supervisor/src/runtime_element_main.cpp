// Test-only finite delays/exceptions. No readiness/heartbeat application calls.
#include "kcf/process/process_runtime.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

class RuntimeTestElement : public kcf::ProcessElement
{
public:
    std::string name{"runtime-test"};
    unsigned setup_delay{0}, setup_result{0}, stall_after{0}, stall_ms{0};
    unsigned throw_after{0}, shutdown_delay{0}, loops{0};
    int Setup() override
    {
        std::cout << "[RuntimeTest] " << name << " Setup begin" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(setup_delay));
        std::cout << "[RuntimeTest] " << name << " Setup end result=" << setup_result << std::endl;
        return static_cast<int>(setup_result);
    }
    void Loop() override
    {
        ++loops;
        if (loops==stall_after)
        {
            std::cout << "[RuntimeTest] " << name << " stall begin" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        }
        if (loops==throw_after) throw std::runtime_error("test Loop exception");
    }
    void Shutdown() override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(shutdown_delay));
        std::cout << "[RuntimeTest] " << name << " Shutdown loops=" << loops << std::endl;
    }
};
int main(int argc,char** argv)
{
    RuntimeTestElement element;
    unsigned frequency=100;
    for(int i=1;i<argc;i+=2)
    {
        if(i+1>=argc) return 2;
        const std::string key=argv[i],text=argv[i+1];
        if(key=="--element-name") { element.name=text; continue; }
        unsigned value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || value>60000) return 2;
        if(key=="--setup-delay-ms") element.setup_delay=value;
        else if(key=="--setup-result") { if(value>255) return 2; element.setup_result=value; }
        else if(key=="--stall-after-loops") element.stall_after=value;
        else if(key=="--stall-ms") element.stall_ms=value;
        else if(key=="--throw-after-loops") element.throw_after=value;
        else if(key=="--shutdown-delay-ms") element.shutdown_delay=value;
        else if(key=="--frequency" && value && value<=10000) frequency=value;
        else return 2;
    }
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(frequency);
    return runtime.Run(element);
}
