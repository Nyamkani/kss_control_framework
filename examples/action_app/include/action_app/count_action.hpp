
#pragma once

#include "kcf/action/action_types.hpp"
#include "kcf/introspection/type_descriptor.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace action_app
{

struct CountGoal
{
    std::uint32_t target_count;
};

struct CountFeedback
{
    std::uint32_t current_count;
    float progress;
};

struct CountResult
{
    std::uint32_t final_count;
    std::int32_t result_code;
};

static_assert(std::is_trivially_copyable_v<CountGoal>);
static_assert(std::is_trivially_copyable_v<CountFeedback>);
static_assert(std::is_trivially_copyable_v<CountResult>);

static_assert(std::is_standard_layout_v<CountGoal>);
static_assert(std::is_standard_layout_v<CountFeedback>);
static_assert(std::is_standard_layout_v<CountResult>);

inline constexpr kcf::ActionEndpointIds IDS{1, 2, 3};

inline std::string StatusName(std::uint16_t port)
{
    return "/example/action/count/" + std::to_string(port);
}

inline bool ParsePort(int argc, char** argv, std::uint16_t& port)
{
    port = 22120;

    if (argc == 1)
        return true;

    if (argc != 2)
        return false;

    const std::string_view text(argv[1]);

    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        port
    );

    return parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size() &&
           port != 0;
}

} // namespace action_app


// =====================================================
// KCF Type Descriptors
// =====================================================

namespace kcf
{

// 1. Action Goal
template<>
struct TypeDescriptorTraits<action_app::CountGoal>
{
    static constexpr bool defined = true;

    static TypeDescriptor Get() noexcept
    {
        using T = action_app::CountGoal;

        auto d = MakeTypeDescriptor<T>(
            "kcf.example.CountGoal"
        );

        d.protocol_version = 1;
        d.field_count = 1;

        d.fields[0] = MakeField<T, std::uint32_t>(
            "target_count",
            offsetof(T, target_count)
        );

        return d;
    }
};


// 2. Action Feedback
template<>
struct TypeDescriptorTraits<action_app::CountFeedback>
{
    static constexpr bool defined = true;

    static TypeDescriptor Get() noexcept
    {
        using T = action_app::CountFeedback;

        auto d = MakeTypeDescriptor<T>(
            "kcf.example.CountFeedback"
        );

        d.protocol_version = 1;
        d.field_count = 2;

        d.fields[0] = MakeField<T, std::uint32_t>(
            "current_count",
            offsetof(T, current_count)
        );

        d.fields[1] = MakeField<T, float>(
            "progress",
            offsetof(T, progress)
        );

        return d;
    }
};


// 3. Action Result
template<>
struct TypeDescriptorTraits<action_app::CountResult>
{
    static constexpr bool defined = true;

    static TypeDescriptor Get() noexcept
    {
        using T = action_app::CountResult;

        auto d = MakeTypeDescriptor<T>(
            "kcf.example.CountResult"
        );

        d.protocol_version = 1;
        d.field_count = 2;

        d.fields[0] = MakeField<T, std::uint32_t>(
            "final_count",
            offsetof(T, final_count)
        );

        d.fields[1] = MakeField<T, std::int32_t>(
            "result_code",
            offsetof(T, result_code)
        );

        return d;
    }
};

} // namespace kcf
