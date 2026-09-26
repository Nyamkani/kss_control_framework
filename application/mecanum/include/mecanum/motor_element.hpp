#pragma once
#include "mecanum/arduino_motor_driver.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/process/process_element.hpp"
#include <mutex>

namespace mecanum {
// Optional test observation; no Topic layout change and no callback-thread I/O.
struct MotorCommandObservation {
    const char* phase;
    std::uint64_t selected_ns, checked_ns, generated_ns, received_ns;
    std::uint64_t latest_generated_ns, latest_received_ns, applied_expires_ns;
    bool fresh, stop_only, zero;
};
struct MotorOptions {
    std::string device; // explicit selection required; never probe physical UARTs
    int baudrate{115200};
    std::string topic_prefix{"/mecanum"};
    std::uint64_t command_timeout_ns{250000000ULL};
    unsigned boot_wait_ms{2500}; // original SerialComm 500 ms + controller 2000 ms
};
class MotorElement final : public kcf::ProcessElement {
public:
    explicit MotorElement(MotorOptions options) : options_(std::move(options)) {}
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
    int ShutdownError() const { return shutdown_error_; }
private:
    friend struct MotorElementTestAccess;
    void ObserveCommand(const char* phase, const CmdVel&, std::uint64_t received,
                        std::uint64_t selected, std::uint64_t checked,
                        bool fresh, bool stop_only, const WheelRpm&);
    void (*command_observer_)(void*, const MotorCommandObservation&){nullptr};
    void* observer_context_{nullptr};
    int AttachCommand();
    int ApplyCommand(bool force, bool stop_only = false);
    int PublishState();
    int UartFailure(int error);
    MotorOptions options_;
    ArduinoMotorDriver driver_;
    Odometry odometry_;
    WheelState wheels_{};
    WheelOdometry odom_{};
    std::mutex command_mutex_;
    CmdVel latest_command_{};
    std::uint64_t latest_received_ns_{0}; // diagnostic only; never extends freshness
    // Subscriber must be destroyed before its callback state above.
    kcf::Subscriber<CmdVel> command_;
    kcf::Publisher<WheelState> wheel_publisher_;
    kcf::Publisher<WheelOdometry> odom_publisher_;
    bool command_open_{false}, wheel_owned_{false}, odom_owned_{false};
    bool protocol_fault_{false};
    bool command_sent_{false}, encoder_next_{true}, odom_pending_{false};
    bool sensor_due_{false}; // reserve a sensor slot after sending a command
    // The transmitted deadline is a refresh hint, not a veto of newer valid input.
    std::uint64_t last_command_ns_{0}, applied_expires_ns_{0};
    std::uint64_t ready_ns_{0};
    int shutdown_error_{0};
};
} // namespace mecanum
