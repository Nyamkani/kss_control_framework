#include <kcf/process/process_runtime.hpp>
#include <kcf/network/compatibility.hpp>

int main()
{
    constexpr kcf::network::FrameworkVersion version{};
    static_assert(version.major == 5 && version.minor == 6 && version.patch == 1);
    kcf::ProcessRuntime runtime;
    return runtime.IsRunning() ? 1 : 0;
}
