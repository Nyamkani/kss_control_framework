#pragma once
#include "mecanum/messages.hpp"
#include <array>

namespace mecanum {
inline constexpr double PI = 3.14159265358979323846;
inline constexpr double WHEEL_RADIUS_MM = 41.0;
inline constexpr double WHEEL_BASE_MM = 190.0;
inline constexpr double WHEEL_SEPARATION_MM = 205.0;
inline constexpr std::uint32_t ENCODER_CPR = 4320;
inline constexpr int MOTOR_SIGN = -1;
inline constexpr std::array<unsigned, 4> WHEEL_TO_ARDUINO = {0, 3, 1, 2};
using Encoders = std::array<std::int32_t, 4>;
using WheelRpm = std::array<float, 4>;

std::uint64_t MonotonicNs();
bool CommandFresh(const CmdVel&, std::uint64_t now, std::uint64_t ready,
                  std::uint64_t timeout_ns);
bool InverseKinematics(const CmdVel&, WheelRpm& raw_rpm);
std::int64_t EncoderDelta(std::int32_t current, std::int32_t previous);

class Odometry {
public:
    void Initialize(const Encoders&, std::uint64_t timestamp);
    WheelOdometry Update(const Encoders&, std::uint64_t timestamp);
    WheelOdometry Invalid(std::uint64_t timestamp);
private:
    Encoders previous_{};
    std::uint64_t previous_time_{0};
    unsigned invalid_count_{0};
    WheelOdometry state_{};
};
} // namespace mecanum
