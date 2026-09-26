// Adapted from kss_mecanum_ros v2.0 arduino_motor_driver.cpp (Apache-2.0).
// Modified: preserve m/e/z/r ASCII protocol and whole-frame validation;
// use fixed wheel arrays, exact ACKs, one 100 ms transaction deadline.
// Unused legacy command queues removed: MotorElement is the sole UART caller.
#include "mecanum/arduino_motor_driver.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <locale>
#include <sstream>

namespace mecanum {
int ArduinoMotorDriver::PrepareStartup() {
    // No command is outstanding here. Require a complete CRLF boundary and
    // 20 ms quiet, with a total 100 ms budget. Do not flush or swallow an OK.
    const auto deadline = MonotonicNs() + 100000000ULL;
    for (;;) {
        const auto quiet_deadline = MonotonicNs() + 20000000ULL;
        std::string frame;
        const int result = serial_.ReadFrame(frame, std::min(deadline, quiet_deadline));
        if (result == -ETIMEDOUT && frame.empty() && quiet_deadline <= deadline) return 0;
        if (result) return result;
        if (!frame.empty() && frame != "Setup Complete!") return -EPROTO;
        if (MonotonicNs() >= deadline) return -ETIMEDOUT;
    }
}
CommandResult ArduinoMotorDriver::Transaction(const std::string& command, std::string& response,
                                    std::uint64_t valid_until) {
    const auto now = MonotonicNs();
    if (valid_until && now >= valid_until) return {-ETIME, SendPhase::NotStarted};
    const auto deadline = now + 100000000ULL;
    const auto send_deadline = valid_until ? std::min<std::uint64_t>(deadline, valid_until) : deadline;
    int result = serial_.SendData(command, send_deadline);
    if (result) return {result, SendPhase::SendAttempted};
    return {serial_.ReadFrame(response, deadline), SendPhase::SendAttempted};
}
CommandResult ArduinoMotorDriver::Ack(const std::string& command, std::uint64_t valid_until) {
    std::string response;
    auto result = Transaction(command, response, valid_until);
    if (!result.error && response != "OK") result.error = -EPROTO;
    return result;
}
int ArduinoMotorDriver::ResetEncoders() { return Ack("r\r").error; }
CommandResult ArduinoMotorDriver::WriteRPM(const WheelRpm& rpm, std::uint64_t valid_until) {
    WheelRpm motors{};
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(rpm[i])) return {-EINVAL, SendPhase::NotStarted};
        motors[WHEEL_TO_ARDUINO[i]] = rpm[i];
    }
    // Original std::to_string produces 6 decimal places; classic locale is explicit.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed;
    out.precision(6);
    out << 'm';
    for (float value : motors) out << ' ' << value;
    out << '\r';
    const auto command = out.str();
    // MCU buffer is 64 bytes including NUL. Reject, do not silently clip targets.
    if (command.size() > 63) return {-EMSGSIZE, SendPhase::NotStarted};
    return Ack(command, valid_until);
}
template<class T>
int ParseWheels(const std::string& frame, std::array<T, 4>& wheels) {
    std::istringstream stream(frame);
    stream.imbue(std::locale::classic());
    std::array<T, 4> motors{};
    for (auto& value : motors)
        if (!(stream >> value) || !std::isfinite(static_cast<double>(value))) return -EPROTO;
    std::string extra;
    if (stream >> extra) return -EPROTO;
    for (unsigned i = 0; i < 4; ++i) wheels[i] = motors[WHEEL_TO_ARDUINO[i]];
    return 0;
}
int ArduinoMotorDriver::ReadEncoders(Encoders& enc) {
    std::string frame;
    const int result = Transaction("e \r", frame).error;
    return result ? result : ParseWheels(frame, enc);
}
int ArduinoMotorDriver::ReadRPM(WheelRpm& rpm) {
    std::string frame;
    const int result = Transaction("z \r", frame).error;
    return result ? result : ParseWheels(frame, rpm);
}
int ArduinoMotorDriver::BestEffortStop() {
    // Terminate any partial command before the zero command. Old ACKs cannot be
    // correlated after a timeout with this protocol, so this path stays unconfirmed.
    serial_.Discard();
    const auto deadline = MonotonicNs() + 100000000ULL;
    const int result = serial_.SendData("\rm 0 0 0 0\r", deadline);
    std::string ignored;
    if (!result) serial_.ReadFrame(ignored, deadline);
    return result ? result : -EIO;
}
} // namespace mecanum
