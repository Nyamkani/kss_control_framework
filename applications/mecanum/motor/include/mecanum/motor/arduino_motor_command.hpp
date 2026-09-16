#pragma once
#include <vector>
namespace mecanum::motor
{
inline constexpr char RESET_ENCODERS='r';
inline constexpr char READ_ENCODERS='e';
inline constexpr char READ_RPMS='z';
inline constexpr char MOTOR_SPEEDS='m';
struct ArduinoCommand
{
    char command;
    std::vector<float> args;
};
} // namespace mecanum::motor
