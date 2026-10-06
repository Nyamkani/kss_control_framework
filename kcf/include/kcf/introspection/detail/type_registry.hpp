#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <string>
namespace kcf::detail
{
inline constexpr char TYPE_REGISTRY_PREFIX[] = "kcf_introspection_type_";
inline constexpr std::size_t MAX_TYPES = 64;
struct TypeRegistrySnapshot {
    std::uint32_t protocol_version{1};
    std::uint32_t struct_size{sizeof(TypeRegistrySnapshot)};
    std::int32_t pid{0};
    std::uint32_t reserved0{0};
    std::uint64_t process_start_ticks{0};
    std::uint32_t type_count{0};
    std::uint32_t reserved1{0};
    TypeDescriptor types[MAX_TYPES]{};
};
static_assert(std::is_trivially_copyable_v<TypeRegistrySnapshot>);
std::string TypeRegistryName(std::int32_t pid,std::uint64_t ticks);
void BeginTypeRegistry(std::int32_t pid,std::uint64_t ticks) noexcept;
void EndTypeRegistry() noexcept;
std::uint64_t RegisterType(const TypeDescriptor& descriptor) noexcept;
template<class T> std::uint64_t RegisterEndpointType() noexcept {
    if constexpr (TypeDescriptorTraits<T>::defined && std::is_standard_layout_v<T> &&
                  std::is_trivially_copyable_v<T> && !std::is_union_v<T>) {
        try {
            const auto descriptor=TypeDescriptorTraits<T>::Get();
            if (descriptor.payload_size!=sizeof(T) || descriptor.payload_alignment!=alignof(T)) return 0;
            return RegisterType(descriptor);
        } catch (...) { return 0; }
    } else return 0;
}
}
