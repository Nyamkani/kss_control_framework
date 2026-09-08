#pragma once
#include <cstdint>
namespace kcf
{
enum class ElementFailureKind : std::uint8_t
{
    NONE, PROCESS_EXIT, RUNTIME_ERROR, STATUS_TIMEOUT, HEARTBEAT_STALL
};
}
