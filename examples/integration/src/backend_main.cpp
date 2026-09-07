#include "integration/integration_element.hpp"
#include "kcf/process/process_runtime.hpp"

int main()
{
    integration::IntegrationElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(100.0);
    return runtime.Run(element);
}
