#pragma once
#include "kcf/action/action_state.hpp"
#include <type_traits>
namespace kcf
{
struct ActionEndpointIds
{
    std::uint16_t goal_service_id, cancel_service_id, result_service_id;
    bool Valid() const { return goal_service_id != cancel_service_id && goal_service_id != result_service_id && cancel_service_id != result_service_id; }
};
struct ActionHeader
{
    std::uint64_t goal_id{0};
    std::uint64_t feedback_sequence{0};
    ActionState state{ActionState::IDLE};
    std::uint8_t cancel_requested{0};
    std::uint8_t reserved[6]{};
};
static_assert(sizeof(ActionHeader) == 24);
template <typename Feedback> struct ActionStatus { ActionHeader header{}; Feedback feedback{}; };
template <typename Goal> struct ActionGoalRequest { std::uint64_t goal_id{0}; Goal goal{}; };
struct ActionGoalResponse
{
    std::uint64_t goal_id{0};
    std::int32_t result_code{0};
    std::uint8_t accepted{0};
    std::uint8_t reserved[3]{};
};
using ActionCancelResponse = ActionGoalResponse;
struct ActionCancelRequest { std::uint64_t goal_id{0}; };
struct ActionResultRequest { std::uint64_t goal_id{0}; };
template <typename Result> struct ActionResultResponse
{
    std::uint64_t goal_id{0};
    ActionState state{ActionState::IDLE};
    std::uint8_t ready{0};
    std::uint8_t reserved[2]{};
    std::int32_t result_code{0};
    Result result{};
};
static_assert(sizeof(ActionGoalResponse) == 16);
static_assert(std::is_trivially_copyable_v<ActionHeader>);
}
