#pragma once
#include "kcf/introspection/type_descriptor.hpp"
namespace kcf::detail {
struct StorageIdentity { std::uint64_t type_id{0}, layout_id{0}; };
// Internal format-v3 layout fingerprint. Names, ordered fields and numeric
// metadata, encoded least-significant byte first. Never hashes object padding.
inline std::uint64_t LayoutId(const TypeDescriptor& d) noexcept {
    std::uint64_t h=14695981039346656037ull;
    const auto byte=[&](unsigned char c){h^=c;h*=1099511628211ull;};
    const auto number=[&](std::uint64_t n){for(int i=0;i<8;++i){byte(n&255);n>>=8;}};
    const auto name=[&](const char* s){while(*s)byte(static_cast<unsigned char>(*s++));byte(0);};
    number(d.type_id);name(d.type_name);number(d.payload_size);number(d.payload_alignment);number(d.field_count);
    for(std::uint32_t i=0;i<d.field_count;++i){const auto& f=d.fields[i];name(f.name);number(static_cast<unsigned>(f.kind));number(f.offset);number(f.element_size);number(f.array_count);}
    return h;
}
template<class T> StorageIdentity DeclaredStorageIdentity() noexcept {
    if constexpr(TypeDescriptorTraits<T>::defined && std::is_standard_layout_v<T> &&
                 std::is_trivially_copyable_v<T> && !std::is_union_v<T>) {
        try {
            const auto d=TypeDescriptorTraits<T>::Get();
            if(d.payload_size==sizeof(T) && d.payload_alignment==alignof(T) && ValidateTypeDescriptor(d))
                return {d.type_id,LayoutId(d)};
        } catch(...) {}
    }
    return {};
}
}
