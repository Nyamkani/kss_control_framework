#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace service_app
{
inline constexpr std::uint16_t PORT = 22210;
inline constexpr std::uint16_t ADD_ID = 1;
inline constexpr char SERVICE_NAME[] = "/example/add";
struct AddRequest { std::int32_t a; std::int32_t b; };
struct AddResponse { std::int32_t result; bool accepted; };
static_assert(std::is_trivially_copyable_v<AddRequest> && std::is_standard_layout_v<AddRequest>);
static_assert(std::is_trivially_copyable_v<AddResponse> && std::is_standard_layout_v<AddResponse>);
}
namespace kcf
{
template<> struct TypeDescriptorTraits<service_app::AddRequest>
{
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept
    {
        using T = service_app::AddRequest;
        auto d = MakeTypeDescriptor<T>("kcf.example.AddRequest");
        d.protocol_version = 1;
        d.field_count = 2;
        d.fields[0] = MakeField<T, std::int32_t>("a", offsetof(T, a));
        d.fields[1] = MakeField<T, std::int32_t>("b", offsetof(T, b));
        return d;
    }
};
template<> struct TypeDescriptorTraits<service_app::AddResponse>
{
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept
    {
        using T = service_app::AddResponse;
        auto d = MakeTypeDescriptor<T>("kcf.example.AddResponse");
        d.protocol_version = 1;
        d.field_count = 2;
        d.fields[0] = MakeField<T, std::int32_t>("result", offsetof(T, result));
        d.fields[1] = MakeField<T, bool>("accepted", offsetof(T, accepted));
        return d;
    }
};
}
