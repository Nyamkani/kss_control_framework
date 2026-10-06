#pragma once
#include "kcf/action/action_types.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
namespace kcf {
// Local observational metadata only; never a Network wire struct.
struct ActionInfo {
    std::uint64_t registration_id{0};
    char name[241]{}; // existing Action status topic is the canonical action name
    std::uint16_t port{0};ActionEndpointIds ids{};
    std::uint64_t goal_type_id{0},feedback_type_id{0},result_type_id{0};
    std::uint32_t goal_request_size{0},goal_offset{0},status_size{0},status_alignment{0},feedback_offset{0},result_response_size{0},result_offset{0};
    detail::StorageIdentity goal_request_identity,goal_response_identity,cancel_request_identity,cancel_response_identity,result_request_identity,result_response_identity,status_identity;
};
static_assert(std::is_trivially_copyable_v<ActionInfo>);
}
