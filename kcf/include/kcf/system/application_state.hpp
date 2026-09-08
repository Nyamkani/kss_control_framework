#pragma once
#include <cstdint>
namespace kcf
{
enum class ApplicationState : std::uint8_t
{
    INITIALIZING, RUNNING, ERROR, SHUTTING_DOWN, RESETTING
};
}
