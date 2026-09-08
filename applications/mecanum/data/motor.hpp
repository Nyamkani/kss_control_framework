#pragma once

#include "common.hpp"

#include <type_traits>

namespace mecanum::data
{

struct VelocityCommand
{
    SampleHeader header;

    float linear_x_mps{0.0f};
    float linear_y_mps{0.0f};
    float linear_z_mps{0.0f};

    float angular_x_rad_s{0.0f};
    float angular_y_rad_s{0.0f};
    float angular_z_rad_s{0.0f};
};

struct OdometryData
{
    SampleHeader header;

    float x_m{0.0f};
    float y_m{0.0f};
    float z_m{0.0f};

    float roll_rad{0.0f};
    float pitch_rad{0.0f};
    float yaw_rad{0.0f};

    float linear_x_mps{0.0f};
    float linear_y_mps{0.0f};
    float linear_z_mps{0.0f};

    float angular_x_rad_s{0.0f};
    float angular_y_rad_s{0.0f};
    float angular_z_rad_s{0.0f};
};

static_assert(std::is_trivially_copyable_v<VelocityCommand>);
static_assert(std::is_standard_layout_v<VelocityCommand>);
static_assert(std::is_trivially_copyable_v<OdometryData>);
static_assert(std::is_standard_layout_v<OdometryData>);

} // namespace mecanum::data
