#pragma once
#include "kcf/introspection/supervisor_info.hpp"
#include "kcf/ipc/shared_channel.hpp"
namespace kcf::detail
{
inline constexpr char SUPERVISOR_REGISTRY_PREFIX[] = "kcf_introspection_supervisor_";
std::string SupervisorRegistryName(std::int32_t pid, std::uint64_t ticks);
// One owning Bringup, called only on its controlling thread. No observer locks.
class SupervisorRegistry
{
public:
    ~SupervisorRegistry() { End(); }
    void Begin() noexcept;
    void Update(SupervisorInfo snapshot) noexcept;
    void End() noexcept;
private:
    SharedChannel<SupervisorInfo> channel_;
    SupervisorInfo current_{};
    bool active_{false}, dirty_{false}, initialized_{false};
};
}
