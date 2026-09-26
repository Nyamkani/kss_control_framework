// Adapted from kss_mecanum_ros v2.0 mecanum_motor_controller.cpp (Apache-2.0).
// Modified: ROS-free functions, fixed-size messages and explicit validity/timestamps.
// Mapping, sign, dimensions, CPR, delta gate and integration equations are preserved.
#include "mecanum/kinematics.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <time.h>

namespace mecanum {
std::uint64_t MonotonicNs() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t)) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return static_cast<std::uint64_t>(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
bool CommandFresh(const CmdVel& c, std::uint64_t now, std::uint64_t ready,
                  std::uint64_t timeout_ns) {
    return c.generated_at_ns != 0 && c.generated_at_ns >= ready &&
        c.generated_at_ns <= now && now - c.generated_at_ns < timeout_ns &&
        std::isfinite(c.vx_m_s) && std::isfinite(c.vy_m_s) && std::isfinite(c.wz_rad_s);
}
bool InverseKinematics(const CmdVel& c, WheelRpm& rpm) {
    const double radius = 0.0005 * (WHEEL_BASE_MM + WHEEL_SEPARATION_MM);
    const double factor = 60.0 / (2.0 * PI * WHEEL_RADIUS_MM / 1000.0) * MOTOR_SIGN;
    const double linear[4] = {
        c.vx_m_s - c.vy_m_s - c.wz_rad_s * radius, // FL, Arduino 1
        c.vx_m_s + c.vy_m_s + c.wz_rad_s * radius, // FR, Arduino 4
        c.vx_m_s + c.vy_m_s - c.wz_rad_s * radius, // RL, Arduino 2
        c.vx_m_s - c.vy_m_s + c.wz_rad_s * radius  // RR, Arduino 3
    };
    WheelRpm candidate{};
    for (unsigned i = 0; i < 4; ++i) {
        const double value = linear[i] * factor;
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) return false;
        candidate[i] = static_cast<float>(value);
    }
    rpm = candidate;
    return true;
}
std::int64_t EncoderDelta(std::int32_t current, std::int32_t previous) {
    std::int64_t delta = static_cast<std::int64_t>(current) - previous;
    if (delta > INT32_MAX) delta -= (1LL << 32);
    else if (delta < INT32_MIN) delta += (1LL << 32);
    return delta;
}
void Odometry::Initialize(const Encoders& enc, std::uint64_t time) {
    previous_ = enc;
    previous_time_ = time;
    invalid_count_ = 0;
    state_ = {};
    state_.timestamp_ns = state_.pose_timestamp_ns = time;
}
WheelOdometry Odometry::Invalid(std::uint64_t time) {
    ++state_.sequence;
    state_.timestamp_ns = time;
    state_.vx_m_s = state_.vy_m_s = state_.wz_rad_s = 0.0;
    state_.valid = state_.resynchronized = false;
    return state_;
}
WheelOdometry Odometry::Update(const Encoders& enc, std::uint64_t time) {
    Invalid(time);
    if (time <= previous_time_) return state_;
    const double dt = (time - previous_time_) / 1.0e9;
    // Original 200 RPM plausibility bound, margin 1.5, resync on 3 invalid frames.
    const double max_delta = (200.0 / 60.0) * ENCODER_CPR * dt * 1.5;
    std::array<double, 4> theta{};
    bool valid = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto delta = EncoderDelta(enc[i], previous_[i]);
        valid = valid && std::abs(static_cast<double>(delta)) <= max_delta;
        theta[i] = delta * (2.0 * PI / ENCODER_CPR) * MOTOR_SIGN;
    }
    if (!valid) {
        if (++invalid_count_ >= 3) {
            previous_ = enc;
            previous_time_ = time;
            invalid_count_ = 0;
            state_.resynchronized = true;
        }
        return state_;
    }
    invalid_count_ = 0;
    previous_ = enc;
    previous_time_ = time;
    const double dtx = (WHEEL_RADIUS_MM / 4000.0) * (theta[0] + theta[1] + theta[2] + theta[3]);
    const double dty = (WHEEL_RADIUS_MM / 4000.0) * (-theta[0] + theta[1] + theta[2] - theta[3]);
    const double radius = 0.5 * (WHEEL_BASE_MM + WHEEL_SEPARATION_MM);
    const double dtz = WHEEL_RADIUS_MM / (4.0 * radius) * (-theta[0] + theta[1] - theta[2] + theta[3]);
    state_.x_m += dtx * std::cos(state_.yaw_rad) - dty * std::sin(state_.yaw_rad);
    state_.y_m += dtx * std::sin(state_.yaw_rad) + dty * std::cos(state_.yaw_rad);
    state_.yaw_rad += dtz;
    if (state_.yaw_rad >= PI) state_.yaw_rad -= 2.0 * PI;
    else if (state_.yaw_rad <= -PI) state_.yaw_rad += 2.0 * PI;
    state_.vx_m_s = dtx / dt;
    state_.vy_m_s = dty / dt;
    state_.wz_rad_s = dtz / dt;
    state_.pose_timestamp_ns = time;
    state_.valid = true;
    return state_;
}
} // namespace mecanum
