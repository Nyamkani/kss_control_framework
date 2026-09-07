#include "dummy/dummy_element.hpp"
#include "kcf/process/process_runtime.hpp"

int main()
{
    DummyElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(2.0);

    return runtime.Run(element);
}
