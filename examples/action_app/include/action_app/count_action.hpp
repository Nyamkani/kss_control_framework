#pragma once
#include "kcf/action/action_types.hpp"
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
namespace action_app
{
struct CountGoal { std::uint32_t target_count; };
struct CountFeedback { std::uint32_t current_count; float progress; };
struct CountResult { std::uint32_t final_count; std::int32_t result_code; };
static_assert(std::is_trivially_copyable_v<CountGoal>);
static_assert(std::is_trivially_copyable_v<CountFeedback>);
static_assert(std::is_trivially_copyable_v<CountResult>);
inline constexpr kcf::ActionEndpointIds IDS{1, 2, 3};
inline std::string StatusName(std::uint16_t port)
{ return "/example/action/count/" + std::to_string(port); }
inline bool ParsePort(int argc, char** argv, std::uint16_t& port)
{
    port = 22120;
    if (argc == 1) return true;
    if (argc != 2) return false;
    const std::string_view text(argv[1]);
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), port);
    return parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && port != 0;
}
}
