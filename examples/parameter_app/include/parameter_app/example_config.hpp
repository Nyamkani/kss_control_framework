#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace parameter_app
{
inline constexpr char PARAMETER_NAME[] = "/example/config";
struct ExampleConfig
{
    bool enabled;
    std::int32_t count;
    float gain;
    float limits[2];
};
static_assert(std::is_trivially_copyable_v<ExampleConfig>);
static_assert(std::is_standard_layout_v<ExampleConfig>);
}

namespace kcf
{
template<> struct TypeDescriptorTraits<parameter_app::ExampleConfig>
{
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept
    {
        using T = parameter_app::ExampleConfig;
        auto descriptor = MakeTypeDescriptor<T>("kcf.example.ExampleConfig");
        descriptor.protocol_version = 1;
        descriptor.field_count = 4;
        descriptor.fields[0] = MakeField<T, bool>("enabled", offsetof(T, enabled));
        descriptor.fields[1] = MakeField<T, std::int32_t>("count", offsetof(T, count));
        descriptor.fields[2] = MakeField<T, float>("gain", offsetof(T, gain));
        descriptor.fields[3] = MakeField<T, float[2]>("limits", offsetof(T, limits));
        return descriptor;
    }
};
}
