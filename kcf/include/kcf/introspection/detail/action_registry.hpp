#pragma once
#include "kcf/introspection/action_info.hpp"
#include <string>
namespace kcf::detail {
inline constexpr char ACTION_REGISTRY_PREFIX[]="kcf_introspection_action_";
inline constexpr std::size_t MAX_ACTIONS=16;
struct ActionRegistrySnapshot {
    std::uint32_t protocol_version{1},struct_size{sizeof(ActionRegistrySnapshot)};
    std::int32_t pid{0};std::uint32_t count{0};std::uint64_t start_ticks{0};
    ActionInfo actions[MAX_ACTIONS]{};
};
std::string ActionRegistryName(std::int32_t,std::uint64_t);
void BeginActionRegistry(std::int32_t,std::uint64_t) noexcept;
void EndActionRegistry() noexcept;
std::uint64_t RegisterAction(ActionInfo) noexcept;
void UnregisterAction(std::uint64_t) noexcept;
void SetActionPortActive(std::uint16_t,bool) noexcept;
}
