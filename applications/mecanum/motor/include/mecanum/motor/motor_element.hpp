#pragma once
#include "mecanum/motor/arduino_motor_driver.hpp"
#include "data/motor.hpp"
#include "kcf/process/process_element.hpp"
#include "kcf/process/execution_mode.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/system/system_status_channel.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace mecanum::motor
{
class MotorElement : public kcf::ProcessElement
{
public:
    MotorElement(kcf::ExecutionMode mode, std::string serial_port, std::int32_t baud_rate);
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
private:
    int SendVelocity(const data::VelocityCommand& command);
    int CalOdomByEncoder();
    int PublishOdometry();
    kcf::ExecutionMode mode_;
    std::string serial_port_;
    std::int32_t baud_rate_;
    ArduinoMotorDriver driver_;
    kcf::Publisher<data::OdometryData> odometry_pub_;
    kcf::Subscriber<data::VelocityCommand> command_sub_;
    kcf::SystemStatusSubscriber status_sub_;
    bool odometry_owned_{false}, odometry_opened_{false};
    bool command_opened_{false}, status_opened_{false}, driver_initialized_{false};
    std::mutex command_mutex_;
    data::VelocityCommand latest_command_{};
    bool command_pending_{false};
    std::atomic<bool> operational_{false}, error_requested_{false};
    bool safe_{false};
    // ROS v2.0 geometry, 45 degree rollers; millimetres.
    double wheel_seperate_{205.0}, wheel_base_{190.0}, wheel_radius_{41.0};
    int is_motor_reversed_{-1};
    std::uint8_t encoder_invalid_count_{0};
    std::int32_t front_left_motor_prev_enc_{0}, front_right_motor_prev_enc_{0};
    std::int32_t rear_left_motor_prev_enc_{0}, rear_right_motor_prev_enc_{0};
    double x_pos_{0.0}, y_pos_{0.0}, w_z_{0.0};
    double vbx_{0.0}, vby_{0.0}, wbz_{0.0};
    std::uint64_t odometry_sequence_{0};
    std::chrono::steady_clock::time_point prev_time_{}, last_odom_update_{};
};
} // namespace mecanum::motor
