#include "mecanum/motor_element.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace mecanum {
int MotorElement::AttachCommand() {
    if (command_open_) return 0;
    const int result = command_.Create(options_.topic_prefix + "/cmd_vel",
        [this](const CmdVel& value) {
            std::lock_guard<std::mutex> lock(command_mutex_);
            latest_command_ = value;
            latest_received_ns_ = MonotonicNs();
        });
    // A producer is optional at startup. Remain at zero and retry from Loop.
    if (result == -ENOENT || result == -EAGAIN) return 0;
    if (result) return result;
    command_open_ = true;
    if (command_.GetDepth() != 1) return -EINVAL;
    return 0;
}
int MotorElement::Setup() {
    if (options_.device.empty() || options_.command_timeout_ns == 0 ||
        options_.command_timeout_ns > 10000000000ULL) return -EINVAL;
    int result = driver_.Initialize(options_.device, options_.baudrate);
    if (result) {
        std::cerr << "UART initialization failed: " << options_.device << " error=" << result << '\n';
        return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options_.boot_wait_ms));
    result = driver_.PrepareStartup();
    if (result) return UartFailure(result);
    result = driver_.WriteRPM({}).error;
    if (result) return UartFailure(result);
    wheels_.command_acknowledged = true;
    last_command_ns_ = MonotonicNs();
    result = driver_.ResetEncoders();
    if (result) return UartFailure(result);
    Encoders encoder{};
    result = driver_.ReadEncoders(encoder);
    if (result) return UartFailure(result);
    wheels_.encoder_timestamp_ns = MonotonicNs();
    wheels_.encoder_valid = true;
    std::copy(encoder.begin(), encoder.end(), wheels_.encoder_counts);
    odometry_.Initialize(encoder, wheels_.encoder_timestamp_ns);
    odom_ = odometry_.Invalid(wheels_.encoder_timestamp_ns);
    odom_pending_ = true;
    result = wheel_publisher_.Create(options_.topic_prefix + "/wheel_state", 1);
    if (result) return result;
    wheel_owned_ = true;
    result = odom_publisher_.Create(options_.topic_prefix + "/wheel_odometry", 1);
    if (result) return result;
    odom_owned_ = true;
    // Do not execute a command created before this new lifecycle was ready.
    ready_ns_ = MonotonicNs();
    result = AttachCommand();
    if (result) return result;
    std::cout << "Motor ready; targets zero; prefix=" << options_.topic_prefix << std::endl;
    return PublishState();
}
void MotorElement::ObserveCommand(const char* phase, const CmdVel& command,
        std::uint64_t received, std::uint64_t selected, std::uint64_t checked,
        bool fresh, bool stop_only, const WheelRpm& target) {
    if (!command_observer_) return;
    MotorCommandObservation observation{phase, selected, checked, command.generated_at_ns,
        received, 0, 0, applied_expires_ns_, fresh, stop_only,
        std::all_of(target.begin(), target.end(), [](float v) { return v == 0; })};
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        observation.latest_generated_ns = latest_command_.generated_at_ns;
        observation.latest_received_ns = latest_received_ns_;
    }
    command_observer_(observer_context_, observation);
}
int MotorElement::ApplyCommand(bool force, bool stop_only) {
    CmdVel command{};
    std::uint64_t received, selected;
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        command = latest_command_;
        received = latest_received_ns_;
        selected = MonotonicNs();
    }
    WheelRpm target{};
    const auto checked = MonotonicNs();
    bool fresh = CommandFresh(command, checked, ready_ns_, options_.command_timeout_ns);
    if (fresh && !InverseKinematics(command, target)) {
        fresh = false;
        target = {};
    }
    const bool applied_expired = applied_expires_ns_ && MonotonicNs() >= applied_expires_ns_;
    ObserveCommand("selected", command, received, selected, checked, fresh, stop_only, target);
    if (stop_only) {
        // The latest input authorizes motion until ITS original generation
        // deadline. Expiry of an older transmitted snapshot cannot invalidate a
        // newer valid input. Defer nonzero updates to preserve the sensor slot.
        const bool zero = std::all_of(target.begin(), target.end(), [](float v) { return v == 0; });
        if (fresh && !zero) return 0;
    }
    if (!force && !applied_expired && wheels_.command_fresh == fresh &&
        std::equal(target.begin(), target.end(), wheels_.target_rpm)) return 0;
    command_sent_ = true;
    std::copy(target.begin(), target.end(), wheels_.target_rpm);
    wheels_.command_fresh = fresh;
    wheels_.command_acknowledged = false;
    const auto expires = fresh ? command.generated_at_ns + options_.command_timeout_ns : 0;
    ObserveCommand("send", command, received, selected, checked, fresh, stop_only, target);
    const auto command_result = driver_.WriteRPM(target, expires);
    int result = command_result.error;
    // Only explicit pre-send rejection permits a normal zero-target fallback.
    // The same errno after SendData (including oversized replies) is ambiguous.
    if (command_result.phase == SendPhase::NotStarted &&
        (result == -ETIME || result == -EMSGSIZE)) {
        fresh = false;
        wheels_.command_fresh = false;
        std::fill(std::begin(wheels_.target_rpm), std::end(wheels_.target_rpm), 0.0f);
        result = driver_.WriteRPM({}).error;
    }
    if (result) return UartFailure(result);
    wheels_.command_acknowledged = true;
    last_command_ns_ = MonotonicNs();
    applied_expires_ns_ = fresh ? expires : 0;
    // Input can change while awaiting ACK. Re-select the latest snapshot before
    // deciding to stop; never decide from the pre-transaction snapshot alone.
    // stop_only permits at most one extra zero transaction, never recursive
    // nonzero updates, so UART work and sensor fairness remain bounded.
    const bool moving = std::any_of(target.begin(), target.end(), [](float v) { return v != 0; });
    if (fresh && moving) return ApplyCommand(false, true);
    return 0;
}
int MotorElement::UartFailure(int error) {
    protocol_fault_ = true;
    wheels_.last_error = error;
    wheels_.encoder_valid = wheels_.rpm_valid = false;
    wheels_.command_fresh = wheels_.command_acknowledged = false;
    std::fill(std::begin(wheels_.target_rpm), std::end(wheels_.target_rpm), 0.0f);
    // UART is still owned by this lifecycle thread. Attempt stopping before
    // diagnostics or IPC; an ACK here never clears the latched protocol fault.
    if (driver_.IsOpen()) driver_.BestEffortStop();
    odom_ = odometry_.Invalid(MonotonicNs());
    odom_pending_ = true;
    std::cerr << "UART transaction failed: " << error << "; stop is unconfirmed\n";
    PublishState(); // diagnostic best effort; preserve the original UART error
    return error; // Runtime exits Loop and calls Shutdown, no further driving
}
int MotorElement::PublishState() {
    wheels_.timestamp_ns = MonotonicNs();
    ++wheels_.sequence;
    int first = 0;
    if (wheel_owned_) {
        const int result = wheel_publisher_.Publish(wheels_);
        if (result && result != -EAGAIN) first = result;
    }
    if (odom_owned_ && odom_pending_) {
        const int result = odom_publisher_.Publish(odom_);
        if (!result) odom_pending_ = false;
        if (result && result != -EAGAIN && !first) first = result;
    }
    return first; // busy latest-value snapshots can be skipped
}
int MotorElement::Loop() {
    command_sent_ = false;
    // After a command slot, defer new nonzero targets for ONE sensor transaction.
    // Stop/expiry still preempts that slot. Callbacks retain only the latest value.
    // The 100 ms refresh threshold applies only to command-eligible slots.
    int result = ApplyCommand(!sensor_due_ && MonotonicNs() - last_command_ns_ >= 100000000ULL,
                              sensor_due_);
    if (result) return result;
    result = AttachCommand();
    if (result) return result;
    if (command_sent_) {
        sensor_due_ = true;
        return PublishState(); // command/stop slot: no sensor I/O
    }
    if (encoder_next_) {
        Encoders encoder{};
        result = driver_.ReadEncoders(encoder);
        if (result) return UartFailure(result);
        wheels_.encoder_timestamp_ns = MonotonicNs();
        wheels_.encoder_valid = true;
        std::copy(encoder.begin(), encoder.end(), wheels_.encoder_counts);
        odom_ = odometry_.Update(encoder, wheels_.encoder_timestamp_ns);
        odom_pending_ = true;
    } else {
        WheelRpm rpm{};
        result = driver_.ReadRPM(rpm);
        if (result) return UartFailure(result);
        wheels_.rpm_timestamp_ns = MonotonicNs();
        wheels_.rpm_valid = true;
        std::copy(rpm.begin(), rpm.end(), wheels_.measured_rpm);
    }
    encoder_next_ = !encoder_next_;
    sensor_due_ = false;
    result = ApplyCommand(false, true);
    if (result) return result;
    wheels_.last_error = 0;
    return PublishState();
}
void MotorElement::Shutdown() {
    int error = 0;
    auto remember = [&error](int result) { if (result && !error) error = result; };
    wheels_.command_fresh = wheels_.command_acknowledged = false;
    std::fill(std::begin(wheels_.target_rpm), std::end(wheels_.target_rpm), 0.0f);
    if (driver_.IsOpen()) {
        const int stopped = protocol_fault_ ? driver_.BestEffortStop() : driver_.WriteRPM({}).error;
        remember(stopped);
        if (stopped) {
            if (!protocol_fault_) driver_.BestEffortStop();
            std::cerr << "STOP UNCONFIRMED: zero target requested, error=" << stopped << '\n';
        } else {
            wheels_.command_acknowledged = true;
            std::cout << "Zero target ACK received (physical stop not verified)\n";
        }
    }
    wheels_.encoder_valid = wheels_.rpm_valid = false;
    wheels_.last_error = error ? error : wheels_.last_error;
    odom_ = odometry_.Invalid(MonotonicNs());
    odom_pending_ = true;
    remember(PublishState());
    if (command_open_) { remember(command_.Close()); command_open_ = false; }
    remember(driver_.Close());
    if (wheel_owned_) {
        remember(wheel_publisher_.Close());
        remember(wheel_publisher_.Unlink());
        wheel_owned_ = false;
    }
    if (odom_owned_) {
        remember(odom_publisher_.Close());
        remember(odom_publisher_.Unlink());
        odom_owned_ = false;
    }
    shutdown_error_ = error;
    if (error) throw std::runtime_error("Motor shutdown failed; error=" + std::to_string(error));
}
} // namespace mecanum
