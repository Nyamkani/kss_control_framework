#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mecanum {
inline constexpr char CMD_VEL_TOPIC[] = "/mecanum/cmd_vel";
inline constexpr char WHEEL_STATE_TOPIC[] = "/mecanum/wheel_state";
inline constexpr char WHEEL_ODOMETRY_TOPIC[] = "/mecanum/wheel_odometry";

// All timestamps: CLOCK_MONOTONIC nanoseconds on this Linux host.
// Every wheel array: FL, FR, RL, RR (Arduino motor 1, 4, 2, 3).
// Plain fixed-size structs: no owning pointers, strings or containers on IPC.
struct CmdVel {
    std::uint64_t sequence;
    std::uint64_t generated_at_ns;
    double vx_m_s; // base_footprint: +x forward, +y left, +yaw CCW
    double vy_m_s;
    double wz_rad_s;
};
struct WheelState {
    std::uint64_t sequence;
    std::uint64_t timestamp_ns; // snapshot publication time
    std::uint64_t encoder_timestamp_ns; // host receipt of complete e response
    std::uint64_t rpm_timestamp_ns; // host receipt of complete z response
    std::int32_t encoder_counts[4]; // raw signed MCU cumulative counts, wrap at 32 bits
    float measured_rpm[4]; // raw MCU sign; multiply by MOTOR_SIGN for body convention
    float target_rpm[4]; // raw MCU sign, requested target (NOT measured motion)
    bool encoder_valid;
    bool rpm_valid;
    bool command_fresh;
    bool command_acknowledged; // ACK only, not proof of physical stop
    std::int32_t last_error; // negative errno, 0 on successful cycle
};
struct WheelOdometry {
    std::uint64_t sequence;
    std::uint64_t timestamp_ns; // encoder sample used/attempted this cycle
    std::uint64_t pose_timestamp_ns; // last accepted integration/baseline time
    double x_m; // odom frame, origin at this Element startup
    double y_m;
    double yaw_rad;
    double vx_m_s; // base_footprint frame
    double vy_m_s;
    double wz_rad_s;
    bool valid; // false: retained pose, zero twist, do not fuse as a measurement
    bool resynchronized; // baseline changed after 3 invalid encoder deltas
};
static_assert(std::is_trivial_v<CmdVel> && std::is_standard_layout_v<CmdVel>);
static_assert(std::is_trivial_v<WheelState> && std::is_standard_layout_v<WheelState>);
static_assert(std::is_trivial_v<WheelOdometry> && std::is_standard_layout_v<WheelOdometry>);
} // namespace mecanum

namespace kcf {
#define MECANUM_FIELD(member) d.fields[d.field_count++] = MakeField<T, decltype(T::member)>(#member, offsetof(T, member))
template<> struct TypeDescriptorTraits<mecanum::CmdVel> {
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept {
        using T = mecanum::CmdVel;
        auto d = MakeTypeDescriptor<T>("mecanum.CmdVel.v1");
        MECANUM_FIELD(sequence); MECANUM_FIELD(generated_at_ns);
        MECANUM_FIELD(vx_m_s); MECANUM_FIELD(vy_m_s); MECANUM_FIELD(wz_rad_s);
        return d;
    }
};
template<> struct TypeDescriptorTraits<mecanum::WheelState> {
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept {
        using T = mecanum::WheelState;
        auto d = MakeTypeDescriptor<T>("mecanum.WheelState.v1");
        MECANUM_FIELD(sequence); MECANUM_FIELD(timestamp_ns);
        MECANUM_FIELD(encoder_timestamp_ns); MECANUM_FIELD(rpm_timestamp_ns);
        MECANUM_FIELD(encoder_counts); MECANUM_FIELD(measured_rpm); MECANUM_FIELD(target_rpm);
        MECANUM_FIELD(encoder_valid); MECANUM_FIELD(rpm_valid);
        MECANUM_FIELD(command_fresh); MECANUM_FIELD(command_acknowledged); MECANUM_FIELD(last_error);
        return d;
    }
};
template<> struct TypeDescriptorTraits<mecanum::WheelOdometry> {
    static constexpr bool defined = true;
    static TypeDescriptor Get() noexcept {
        using T = mecanum::WheelOdometry;
        auto d = MakeTypeDescriptor<T>("mecanum.WheelOdometry.v1");
        MECANUM_FIELD(sequence); MECANUM_FIELD(timestamp_ns); MECANUM_FIELD(pose_timestamp_ns);
        MECANUM_FIELD(x_m); MECANUM_FIELD(y_m); MECANUM_FIELD(yaw_rad);
        MECANUM_FIELD(vx_m_s); MECANUM_FIELD(vy_m_s); MECANUM_FIELD(wz_rad_s);
        MECANUM_FIELD(valid); MECANUM_FIELD(resynchronized);
        return d;
    }
};
#undef MECANUM_FIELD
} // namespace kcf
