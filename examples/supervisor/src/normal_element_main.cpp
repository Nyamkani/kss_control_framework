#include "kcf/process/process_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include <iostream>
#include <string>

class NormalElement : public kcf::ProcessElement
{
public:
    std::string name{"normal"};
    int Setup() override { std::cout << "[Normal] " << name << " Setup" << std::endl; return 0; }
    int Loop() override { std::cout << "[Normal] " << name << " Loop" << std::endl; return 0; }
    void Shutdown() override { std::cout << "[Normal] " << name << " Shutdown" << std::endl; }
};
int main(int argc, char** argv)
{
    NormalElement element;
    if (argc == 3 && std::string(argv[1]) == "--element-name") element.name = argv[2];
    else if (argc != 1) return 2;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(10.0);
    return runtime.Run(element);
}
