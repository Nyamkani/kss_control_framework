#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace kcf
{
enum class FieldValueKind : std::uint8_t {
    BOOL = 1, INT8, UINT8, INT16, UINT16, INT32, UINT32,
    INT64, UINT64, FLOAT32, FLOAT64
};
struct TypeFieldInfo {
    char name[96]{};
    FieldValueKind kind{};
    std::uint8_t reserved[3]{};
    std::uint32_t offset{0};
    std::uint32_t element_size{0};
    std::uint32_t array_count{1};
};
inline constexpr std::size_t MAX_TYPE_FIELDS = 128;
struct TypeDescriptor {
    std::uint32_t protocol_version{1};
    std::uint32_t struct_size{sizeof(TypeDescriptor)};
    std::uint64_t type_id{0};
    char type_name[160]{};
    std::uint32_t payload_size{0};
    std::uint32_t payload_alignment{0};
    std::uint32_t field_count{0};
    std::uint32_t reserved{0};
    TypeFieldInfo fields[MAX_TYPE_FIELDS]{};
};
static_assert(std::is_trivially_copyable_v<TypeFieldInfo>);
static_assert(std::is_trivially_copyable_v<TypeDescriptor>);
static_assert(sizeof(TypeFieldInfo)==112);
static_assert(sizeof(TypeDescriptor)==192+112*MAX_TYPE_FIELDS);

// Public contract: FNV-1a 64-bit over canonical name bytes (no terminating NUL,
// normalization or locale transform), modulo 2^64. Not cryptographic or
// collision-free. Version canonical names on incompatible contract changes.
constexpr std::uint64_t StableTypeId(std::string_view canonical) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : canonical) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ull; }
    return hash;
}
constexpr std::uint32_t PrimitiveSize(FieldValueKind kind) noexcept {
    switch (kind) {
    case FieldValueKind::BOOL: case FieldValueKind::INT8: case FieldValueKind::UINT8: return 1;
    case FieldValueKind::INT16: case FieldValueKind::UINT16: return 2;
    case FieldValueKind::INT32: case FieldValueKind::UINT32: case FieldValueKind::FLOAT32: return 4;
    case FieldValueKind::INT64: case FieldValueKind::UINT64: case FieldValueKind::FLOAT64: return 8;
    }
    return 0;
}
template<class T> struct TypeDescriptorTraits { static constexpr bool defined = false; };

namespace detail {
template<class T> constexpr FieldValueKind PrimitiveKind() noexcept {
    if constexpr (std::is_same_v<T,bool>) return FieldValueKind::BOOL;
    else if constexpr (std::is_same_v<T,std::int8_t>) return FieldValueKind::INT8;
    else if constexpr (std::is_same_v<T,std::uint8_t>) return FieldValueKind::UINT8;
    else if constexpr (std::is_same_v<T,std::int16_t>) return FieldValueKind::INT16;
    else if constexpr (std::is_same_v<T,std::uint16_t>) return FieldValueKind::UINT16;
    else if constexpr (std::is_same_v<T,std::int32_t>) return FieldValueKind::INT32;
    else if constexpr (std::is_same_v<T,std::uint32_t>) return FieldValueKind::UINT32;
    else if constexpr (std::is_same_v<T,std::int64_t>) return FieldValueKind::INT64;
    else if constexpr (std::is_same_v<T,std::uint64_t>) return FieldValueKind::UINT64;
    else if constexpr (std::is_same_v<T,float> && sizeof(T)==4 && std::numeric_limits<T>::is_iec559) return FieldValueKind::FLOAT32;
    else if constexpr (std::is_same_v<T,double> && sizeof(T)==8 && std::numeric_limits<T>::is_iec559) return FieldValueKind::FLOAT64;
    else return static_cast<FieldValueKind>(0);
}
}
// Explicit offset may also be offsetof(Outer, inner)+offsetof(Inner, member)
// for a manually flattened field. No arbitrary C++ reflection is performed.
template<class Owner, class Member>
constexpr TypeFieldInfo MakeField(std::string_view name, std::size_t offset) noexcept {
    static_assert(std::is_standard_layout_v<Owner> && std::is_trivially_copyable_v<Owner> && !std::is_union_v<Owner>);
    using Value = std::remove_cv_t<std::remove_extent_t<Member>>;
    constexpr auto kind = detail::PrimitiveKind<Value>();
    static_assert(static_cast<unsigned>(kind)!=0, "Only supported primitives and fixed primitive arrays are allowed");
    static_assert(std::rank_v<Member> <= 1 && (!std::is_array_v<Member> || std::extent_v<Member> > 0));
    static_assert(sizeof(Value)==PrimitiveSize(kind), "Unsupported primitive representation");
    TypeFieldInfo field{};
    // Never silently truncate a name or offset: leave an invalid descriptor.
    if (name.empty() || name.size() >= sizeof(field.name) || offset > UINT32_MAX) return field;
    for (std::size_t i=0;i<name.size();++i) { if (!name[i]) return TypeFieldInfo{}; field.name[i]=name[i]; }
    field.kind=kind; field.offset=static_cast<std::uint32_t>(offset); field.element_size=sizeof(Value);
    if constexpr (std::is_array_v<Member>) {
        static_assert(std::extent_v<Member> <= UINT32_MAX);
        field.array_count=std::extent_v<Member>;
    }
    return field;
}
template<class T>
constexpr TypeDescriptor MakeTypeDescriptor(std::string_view canonical) noexcept {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && !std::is_union_v<T>);
    static_assert(sizeof(T)<=UINT32_MAX && alignof(T)<=UINT32_MAX);
    TypeDescriptor descriptor{};
    if (canonical.empty() || canonical.size() >= sizeof(descriptor.type_name)) return descriptor;
    for(std::size_t i=0;i<canonical.size();++i) {
        if (!canonical[i]) return TypeDescriptor{};
        descriptor.type_name[i]=canonical[i];
    }
    descriptor.type_id=StableTypeId(canonical);
    descriptor.payload_size=sizeof(T); descriptor.payload_alignment=alignof(T);
    return descriptor;
}
// Validates untrusted flat metadata, including ranges, widths, names/overlaps.
// This is not proof that an explicit declaration accurately describes arbitrary T.
bool ValidateTypeDescriptor(const TypeDescriptor& descriptor) noexcept;
}
