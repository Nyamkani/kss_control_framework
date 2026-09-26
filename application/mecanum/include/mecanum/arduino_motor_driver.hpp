#pragma once
#include "mecanum/kinematics.hpp"
#include "mecanum/serial_uart.hpp"

namespace mecanum {
// SendAttempted is conservative: SendData was entered, so zero/partial/full
// transmission is possible. Only NotStarted proves a local pre-send rejection.
enum class SendPhase { NotStarted, SendAttempted };
struct CommandResult {
    int error;
    SendPhase phase;
};
class ArduinoMotorDriver {
public:
    int Initialize(const std::string& device, int baudrate) { return serial_.Open(device, baudrate); }
    int Close() { return serial_.Close(); }
    bool IsOpen() const { return serial_.IsOpen(); }
    // Startup only, before the first command: consume complete known boot lines.
    // Never use this to recover an uncertain command/ACK stream.
    int PrepareStartup();
    int ResetEncoders();
    CommandResult WriteRPM(const WheelRpm& raw_wheel_rpm, std::uint64_t valid_until_ns = 0);
    int ReadEncoders(Encoders&);
    int ReadRPM(WheelRpm&);
    // On an ambiguous/failed transaction: send stop, but NEVER claim confirmed ACK.
    int BestEffortStop();
private:
    CommandResult Transaction(const std::string&, std::string&, std::uint64_t valid_until_ns = 0);
    CommandResult Ack(const std::string&, std::uint64_t valid_until_ns = 0);
    SerialComm serial_;
};
} // namespace mecanum
