#include "kcf/introspection/detail/supervisor_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <cstring>
#include <limits>
#include <unistd.h>
namespace kcf::detail
{
std::string SupervisorRegistryName(std::int32_t pid, std::uint64_t ticks)
{
    return std::string("/") + SUPERVISOR_REGISTRY_PREFIX + std::to_string(pid) + "_" + std::to_string(ticks);
}
void SupervisorRegistry::Begin() noexcept
{
    try
    {
        if (active_) return;
        current_ = {};
        current_.pid = getpid();
        if (!ReadProcessIdentity(current_.pid, current_.process_start_ticks)) return;
        active_ = channel_.TryCreate(SupervisorRegistryName(current_.pid, current_.process_start_ticks)) == 0;
        initialized_ = false; dirty_ = active_;
    }
    catch (...) {}
}
void SupervisorRegistry::Update(SupervisorInfo snapshot) noexcept
{
    if (!active_) return;
    snapshot.pid = current_.pid;
    snapshot.process_start_ticks = current_.process_start_ticks;
    snapshot.revision = current_.revision;
    // All fields/reserved bytes are value-initialized, with no implicit padding
    // in the supported Linux layout (sizes asserted in the public model).
    if (!initialized_ || std::memcmp(&snapshot, &current_, sizeof(snapshot)) != 0)
    {
        if (current_.revision == std::numeric_limits<std::uint64_t>::max()) return;
        snapshot.revision++;
        current_ = snapshot; dirty_ = true; initialized_ = true;
    }
    if (dirty_ && channel_.TryPublishSnapshot(current_) == 0) dirty_ = false;
}
void SupervisorRegistry::End() noexcept
{
    if (!active_) return;
    (void)channel_.Close(); (void)channel_.Unlink();
    active_ = false; dirty_ = false; initialized_ = false;
}
}
