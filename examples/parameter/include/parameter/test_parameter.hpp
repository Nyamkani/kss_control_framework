#pragma once
#include <cstdint>
#include <type_traits>

struct ControlParameter
{
    double kp;
    double ki;
    double kd;
    std::uint32_t mode;
};
static_assert(std::is_trivially_copyable_v<ControlParameter>);
