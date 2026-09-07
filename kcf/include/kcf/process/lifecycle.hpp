#pragma once

#include <cstdint>

namespace kcf
{

enum class ProcessState : std::uint8_t
{
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING,
    ERROR
};

} // namespace kcf
