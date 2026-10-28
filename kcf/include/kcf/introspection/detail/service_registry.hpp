#pragma once
#include "kcf/introspection/service_info.hpp"
#include "kcf/introspection/detail/type_registry.hpp"
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
// Process-local token; not observable in SHM until snapshot commit succeeds.
// Existing RegisterService keeps its zero-on-failure contract.
enum class ServiceRegistrationFailure { NONE, INACTIVE, STOPPING, INVALID, DUPLICATE, CAPACITY, STORAGE_CREATE, PUBLISH, RESOURCE };
struct ServiceTypeRegistrationStatus {
    bool supported{false}, pending{false};
    TypeRegistrationResult initial{}, latest{};
    unsigned recovery_attempts{0};
};
struct ServiceRegistrationStatus {
    int initial_error{0}, error{0};
    ServiceRegistrationFailure failure{ServiceRegistrationFailure::NONE};
    unsigned recovery_attempts{0};
    bool pending{false}, recovered{false};
    std::uint64_t registration_id{0};
    ServiceTypeRegistrationStatus request_type{}, response_type{};
};
std::uint64_t RegisterRecoverableService(ServiceInfo info, ServiceRegistrationStatus* status=nullptr) noexcept;
// Descriptors are copied only while retryable; nullptr means intentional legacy 0.
std::uint64_t RegisterTypedService(ServiceInfo info, const TypeDescriptor* request,
    const TypeDescriptor* response, ServiceRegistrationStatus* status=nullptr) noexcept;
bool GetServiceRegistrationStatus(std::uint64_t token, ServiceRegistrationStatus& status) noexcept;
void UnregisterService(std::uint64_t id) noexcept;
}
