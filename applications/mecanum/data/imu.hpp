#pragma once

#include "common.hpp"

#include <cstdint>
#include <type_traits>

namespace mecanum::data
{

enum ImuValid : std::uint32_t
{
    IMU_VALID_NONE  = 0,
    IMU_VALID_ACCEL = 1u << 0,
    IMU_VALID_GYRO  = 1u << 1,
    IMU_VALID_ANGLE = 1u << 2,
    IMU_VALID_MAG   = 1u << 3,
};

struct ImuData
{
    SampleHeader header;

    // Axis order: X, Y, Z. Convert WT901C gyro to rad/s before publishing.
    float accel_g[3]{};
    float gyro_rad_s[3]{};
    // Roll, pitch, yaw in radians; convert before publishing.
    float euler_rad[3]{};

    // X, Y, Z magnetometer raw values.
    std::int32_t mag_raw[3]{};

    std::uint32_t valid_mask{IMU_VALID_NONE};
};

static_assert(std::is_trivially_copyable_v<ImuData>);
static_assert(std::is_standard_layout_v<ImuData>);

} // namespace mecanum::data
