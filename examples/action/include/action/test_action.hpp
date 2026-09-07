#pragma once
#include "kcf/action/action_types.hpp"

struct CountGoal { std::uint32_t target_count; };
struct CountFeedback { std::uint32_t current_count; float progress; };
struct CountResult { std::uint32_t final_count; std::int32_t result_code; };
constexpr kcf::ActionEndpointIds COUNT_IDS{100, 101, 102};
constexpr const char* COUNT_STATUS = "/kcf_test_count_action/status";
static_assert(std::is_trivially_copyable_v<CountGoal> && std::is_trivially_copyable_v<CountFeedback> && std::is_trivially_copyable_v<CountResult>);
