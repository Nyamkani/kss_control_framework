#pragma once
#include "kcf/introspection/endpoint_info.hpp"
#include "kcf/introspection/detail/type_registry.hpp"
#include <cstddef>
#include <string>
namespace kcf::detail
{
inline constexpr char ENDPOINT_REGISTRY_PREFIX[] = "kcf_introspection_endpoint_";
inline constexpr std::size_t MAX_ENDPOINTS = 128;
struct EndpointRegistrySnapshot
{
    std::uint32_t protocol_version{2};
    std::uint32_t struct_size{sizeof(EndpointRegistrySnapshot)};
    std::int32_t pid{0};
    std::uint64_t process_start_ticks{0};
    std::uint64_t revision{0};
    std::uint32_t endpoint_count{0};
    EndpointInfo endpoints[MAX_ENDPOINTS]{};
};
static_assert(std::is_trivially_copyable_v<EndpointRegistrySnapshot>);
std::string EndpointRegistryName(std::int32_t pid, std::uint64_t ticks);
void BeginEndpointRegistry(std::int32_t pid, std::uint64_t ticks) noexcept;
void EndEndpointRegistry() noexcept;
std::uint64_t RegisterEndpoint(EndpointKind kind, EndpointRole role,
    const std::string& name, std::uint64_t payload_size, const char* diagnostic_type_name, std::uint64_t type_id = 0) noexcept;
void UnregisterEndpoint(std::uint64_t id) noexcept;
// Bounded, process-local recovery diagnostics. Initial failure is retained after
// recovery. A missing/unregistered ID returns false. No IPC ABI is added.
struct EndpointTypeRegistrationStatus {
    TypeRegistrationResult initial{};
    TypeRegistrationResult latest{};
    unsigned recovery_attempts{0};
    bool pending{false};
    bool recovered{false};
};
bool GetEndpointTypeRegistrationStatus(std::uint64_t id, EndpointTypeRegistrationStatus& out) noexcept;
std::uint64_t RegisterEndpointWithType(EndpointKind kind, EndpointRole role,
    const std::string& name, std::uint64_t size, const char* diagnostic,
    const TypeDescriptor* descriptor) noexcept;
template<class T> std::uint64_t RegisterTypedEndpoint(EndpointKind kind, EndpointRole role,
    const std::string& name, const char* diagnostic) noexcept {
    if constexpr (TypeDescriptorTraits<T>::defined && std::is_standard_layout_v<T> &&
                  std::is_trivially_copyable_v<T> && !std::is_union_v<T>) {
        try {
            auto descriptor=TypeDescriptorTraits<T>::Get();
            // Preserve an observable invalid-descriptor result for layout mismatch.
            if(descriptor.payload_size!=sizeof(T) || descriptor.payload_alignment!=alignof(T))
                descriptor.type_id=0;
            return RegisterEndpointWithType(kind,role,name,sizeof(T),diagnostic,&descriptor);
        } catch (...) { return RegisterEndpoint(kind,role,name,sizeof(T),diagnostic,0); }
    } else return RegisterEndpoint(kind,role,name,sizeof(T),diagnostic,0);
}
// Internal compiler-specific diagnostic text; no allocation or stable format.
template<class T> const char* DiagnosticTypeName() noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    return __PRETTY_FUNCTION__;
#else
    return "unknown type";
#endif
}
}
