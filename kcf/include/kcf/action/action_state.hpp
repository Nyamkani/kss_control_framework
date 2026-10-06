#pragma once
#include <cstdint>
namespace kcf
{
enum class ActionState : std::uint8_t { IDLE, ACCEPTED, RUNNING, SUCCEEDED, FAILED, CANCELED };
inline bool IsActionTerminal(ActionState state)
{
    return state == ActionState::SUCCEEDED || state == ActionState::FAILED || state == ActionState::CANCELED;
}
}
