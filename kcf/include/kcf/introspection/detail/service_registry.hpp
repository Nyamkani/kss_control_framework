#pragma once
#include "kcf/introspection/service_info.hpp"
#include <cstddef>
#include <string>
namespace kcf::detail {
inline constexpr char SERVICE_REGISTRY_PREFIX[]="kcf_introspection_service_";
inline constexpr std::size_t MAX_SERVICES=64;
struct ServiceRegistrySnapshot {
    std::uint32_t protocol_version{1}, struct_size{sizeof(ServiceRegistrySnapshot)};
    std::int32_t pid{0};std::uint32_t reserved0{0};
    std::uint64_t process_start_ticks{0}, revision{0};
    std::uint32_t service_count{0}, reserved1{0};
    ServiceInfo services[MAX_SERVICES]{};
};
static_assert(std::is_trivially_copyable_v<ServiceRegistrySnapshot>);
std::string ServiceRegistryName(std::int32_t pid,std::uint64_t ticks);
void BeginServiceRegistry(std::int32_t pid,std::uint64_t ticks) noexcept;
void EndServiceRegistry() noexcept;
std::uint64_t RegisterService(ServiceInfo info) noexcept;
void UnregisterService(std::uint64_t id) noexcept;
}
