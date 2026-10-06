#pragma once

#include "kcf/process/runtime_supervision.hpp"

#include <cstdint>
#include <cstdlib>

namespace kcf
{

enum class ExecutionMode : std::uint8_t
{
    STANDALONE = 0,
    SUPERVISED = 1
};

// Call before ProcessRuntime::Run consumes the environment variable.
// Presence indicates launch intent; StartSupervision validates the actual FD.
inline ExecutionMode DetectLaunchExecutionMode() noexcept
{
    return std::getenv(SUPERVISION_FD_ENV) ? ExecutionMode::SUPERVISED : ExecutionMode::STANDALONE;
}

} // namespace kcf
