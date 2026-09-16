#pragma once
#include "mecanum/motor/serial_uart.hpp"
#include "mecanum/motor/arduino_motor_command.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace mecanum::motor
{
struct MotorData
{
    std::int32_t encoder{0};
    float rpm{0.0f};
};
class ArduinoMotorDriver
{
public:
    int Initialize(const std::string& tty, std::int32_t baud_rate);
    int Close();
    int Write(const ArduinoCommand& cmd);
    int Read(const ArduinoCommand& cmd);
    float ReadRPM(int motor) const;
    std::int32_t ReadEncoder(int motor) const;
private:
    SerialComm Serial_;
    static constexpr int no_of_motors_=4;
    std::vector<MotorData> motor_data_{4};
};
} // namespace mecanum::motor
