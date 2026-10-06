#pragma once
#include "kcf/action/action_types.hpp"

namespace integration
{
constexpr std::uint16_t SERVICE_PORT = 22100;
constexpr std::uint16_t SERVICE_ADD = 1, SERVICE_SET_VALUE = 2;
constexpr kcf::ActionEndpointIds ACTION_IDS{100, 101, 102};
constexpr const char* COMMAND_TOPIC = "/kcf_integration/command";
constexpr const char* STATE_TOPIC = "/kcf_integration/state";
constexpr const char* GAIN_PARAMETER = "/kcf_integration/gain";
constexpr const char* ACTION_TOPIC = "/kcf_integration/count_action/status";
struct IntegrationCommand { std::int32_t target; };
struct IntegrationState
{
    std::uint64_t sequence{0};
    std::int32_t target{0};
    std::int32_t service_value{0};
    double gain{1.0};
    std::uint32_t action_count{0};
};
struct AddRequest { std::int32_t a, b; };
struct AddResponse { std::int32_t sum; };
struct SetValueRequest { std::int32_t value; };
struct SetValueResponse { std::int32_t value; };
struct CountGoal { std::uint32_t target_count; };
struct CountFeedback { std::uint32_t current_count; float progress; };
struct CountResult { std::uint32_t final_count; std::int32_t result_code; };
} // namespace integration
