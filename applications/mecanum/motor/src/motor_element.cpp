// Encoder and inverse kinematics ported from kss_mecanum_ros v2.0 active path.
#include "mecanum/motor/motor_element.hpp"
#include "common/time.hpp"
#include <cerrno>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mecanum::motor
{
static constexpr unsigned MOTOR_ENC_CNT=4320;
static constexpr double PI=3.14159265358979323846;
static int64_t EncoderDelta(
    int32_t current,
    int32_t previous)
{
    int64_t delta =
        static_cast<int64_t>(current) -
        static_cast<int64_t>(previous);

    constexpr int64_t ENC_RANGE =
        (1LL << 32);

    if(delta > INT32_MAX)
    {
        delta -= ENC_RANGE;
    }
    else if(delta < INT32_MIN)
    {
        delta += ENC_RANGE;
    }

    return delta;
}


MotorElement::MotorElement(kcf::ExecutionMode mode, std::string serial_port, std::int32_t baud_rate)
    : mode_(mode), serial_port_(std::move(serial_port)), baud_rate_(baud_rate)
{
    operational_ = mode_ == kcf::ExecutionMode::STANDALONE;
}

int MotorElement::Setup()
{
    int result=odometry_pub_.Create(data::MOTOR_ODOMETRY_TOPIC);
    if(result) return result;
    odometry_owned_=odometry_opened_=true;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do
    {
        result=command_sub_.Create(data::MOTOR_COMMAND_TOPIC,[this](const data::VelocityCommand& command)
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            latest_command_=command;
            command_pending_=true;
        });
        if(result!= -ENOENT && result!= -EAGAIN) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while(std::chrono::steady_clock::now()<deadline);
    if(result) return result;
    command_opened_=true;
    if(mode_==kcf::ExecutionMode::SUPERVISED)
    {
        result=status_sub_.Open([this](const kcf::SystemStatus& status)
        {
            operational_=status.state==kcf::ApplicationState::RUNNING;
            if(status.state==kcf::ApplicationState::ERROR) error_requested_=true;
        });
        if(result) return result;
        status_opened_=true;
    }
    result=driver_.Initialize(serial_port_,baud_rate_);
    if(result) return result;
    driver_initialized_=true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if(driver_.Write({RESET_ENCODERS,{}})!=0) return -EIO;
    if(driver_.Read({READ_ENCODERS,{}})!=0) return -EIO;
    front_left_motor_prev_enc_=driver_.ReadEncoder(1);
    front_right_motor_prev_enc_=driver_.ReadEncoder(4);
    rear_left_motor_prev_enc_=driver_.ReadEncoder(2);
    rear_right_motor_prev_enc_=driver_.ReadEncoder(3);
    prev_time_=last_odom_update_=std::chrono::steady_clock::now();
    return 0;
}

int MotorElement::Loop()
{
    if(mode_==kcf::ExecutionMode::SUPERVISED && error_requested_.load() && !safe_)
    {
        safe_=true;
        if(driver_.Write({MOTOR_SPEEDS,{0,0,0,0}})!=0) return -EIO;
    }
    data::VelocityCommand command{};
    bool pending=false;
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if(!safe_ && operational_.load() && command_pending_)
        {
            command=latest_command_;
            pending=true;
        }
        command_pending_=false; // discard non-operational commands
    }
    if(pending)
    {
        const int result=SendVelocity(command);
        if(result) return result;
    }
    if(std::chrono::steady_clock::now()-last_odom_update_>=std::chrono::milliseconds(15))
        return CalOdomByEncoder();
    return 0;
}

int MotorElement::SendVelocity(const data::VelocityCommand& command)
{
    ArduinoCommand cmd;

    // Application velocity
    // linear  : m/s
    // angular : rad/s
    const double vbx = command.linear_x_mps;
    const double vby = command.linear_y_mps;
    const double wbz = command.angular_z_rad_s;

    // wheel_base_, wheel_seperate_ are full dimensions [mm]
    // distance from robot center to wheel [m]
    const double rotation_radius =
        0.0005 * (this->wheel_base_ + this->wheel_seperate_);

    // Mecanum inverse kinematics
    // result: wheel linear velocity [m/s]
    const double motor_speed_1 =
        vbx - vby - wbz * rotation_radius;

    const double motor_speed_2 =
        vbx + vby + wbz * rotation_radius;

    const double motor_speed_3 =
        vbx - vby + wbz * rotation_radius;

    const double motor_speed_4 =
        vbx + vby - wbz * rotation_radius;

    // wheel circumference [m]
    const double wheel_length =
        2.0 * PI * this->wheel_radius_ / 1000.0;

    // wheel linear velocity [m/s]
    // -> rev/s
    // -> RPM
    const double rpm_from_vel =
        60.0 / wheel_length;

    const float m1_rpm =
        static_cast<float>(
            motor_speed_1 *
            rpm_from_vel *
            this->is_motor_reversed_);

    const float m2_rpm =
        static_cast<float>(
            motor_speed_2 *
            rpm_from_vel *
            this->is_motor_reversed_);

    const float m3_rpm =
        static_cast<float>(
            motor_speed_3 *
            rpm_from_vel *
            this->is_motor_reversed_);

    const float m4_rpm =
        static_cast<float>(
            motor_speed_4 *
            rpm_from_vel *
            this->is_motor_reversed_);

    cmd.command = 'm';

    // Arduino motor mapping
    cmd.args.push_back(m1_rpm); // Arduino motor 1
    cmd.args.push_back(m4_rpm); // Arduino motor 2
    cmd.args.push_back(m3_rpm); // Arduino motor 3
    cmd.args.push_back(m2_rpm); // Arduino motor 4

    if(driver_.Write(cmd) < 0)
    {
        return -EIO;
    }
    return 0;
}

int MotorElement::CalOdomByEncoder()
{
    // ---------------------------------------------------------
    // 1. Read encoder
    // ---------------------------------------------------------
    ArduinoCommand cmd;

    cmd.command = 'e';
    cmd.args.clear();

    if(driver_.Read(cmd) < 0)
    {
        return -EIO;
    }

    const int32_t front_left_motor_enc =
        driver_.ReadEncoder(1);

    const int32_t front_right_motor_enc =
        driver_.ReadEncoder(4);

    const int32_t rear_left_motor_enc =
        driver_.ReadEncoder(2);

    const int32_t rear_right_motor_enc =
        driver_.ReadEncoder(3);


    // ---------------------------------------------------------
    // 2. Calculate elapsed time
    // ---------------------------------------------------------
    const auto now =
        std::chrono::steady_clock::now();
    last_odom_update_=now;

    const double dt_sec =
        std::chrono::duration<double>(
            now - this->prev_time_).count();

    if(dt_sec <= 0.0)
    {
        return 0;
    }


    // ---------------------------------------------------------
    // 3. Calculate encoder delta
    //    EncoderDelta() handles int32_t wrap-around.
    // ---------------------------------------------------------
    const int64_t front_left_delta_enc =
        EncoderDelta(
            front_left_motor_enc,
            this->front_left_motor_prev_enc_);

    const int64_t front_right_delta_enc =
        EncoderDelta(
            front_right_motor_enc,
            this->front_right_motor_prev_enc_);

    const int64_t rear_left_delta_enc =
        EncoderDelta(
            rear_left_motor_enc,
            this->rear_left_motor_prev_enc_);

    const int64_t rear_right_delta_enc =
        EncoderDelta(
            rear_right_motor_enc,
            this->rear_right_motor_prev_enc_);


    // ---------------------------------------------------------
    // 4. Encoder sanity check
    //
    // maximum allowable encoder movement is calculated from:
    //
    // RPM -> rev/s -> encoder count / dt
    //
    // 200 RPM is intentionally larger than normal driving speed.
    // ---------------------------------------------------------
    constexpr double ODOM_MAX_WHEEL_RPM = 200.0;
    constexpr double ODOM_DELTA_MARGIN = 1.5;

    const double max_delta_enc =
        (ODOM_MAX_WHEEL_RPM / 60.0)
        * static_cast<double>(MOTOR_ENC_CNT)
        * dt_sec
        * ODOM_DELTA_MARGIN;

    const auto is_delta_valid =
        [max_delta_enc](int64_t delta)
        {
            return std::abs(
                static_cast<double>(delta))
                <= max_delta_enc;
        };

    constexpr uint8_t ODOM_RESYNC_INVALID_COUNT = 3;

    const bool encoder_delta_valid =
        is_delta_valid(front_left_delta_enc)  &&
        is_delta_valid(front_right_delta_enc) &&
        is_delta_valid(rear_left_delta_enc)   &&
        is_delta_valid(rear_right_delta_enc);

    if(!encoder_delta_valid)
    {
        this->encoder_invalid_count_++;

        if(this->encoder_invalid_count_ >=
        ODOM_RESYNC_INVALID_COUNT)
        {
            // Persistent invalid delta:
            // assume encoder reference has changed
            // (e.g. MCU reboot / encoder reset).
            //
            // Re-sync only the Element-side baseline.
            this->front_left_motor_prev_enc_ =
                front_left_motor_enc;

            this->front_right_motor_prev_enc_ =
                front_right_motor_enc;

            this->rear_left_motor_prev_enc_ =
                rear_left_motor_enc;

            this->rear_right_motor_prev_enc_ =
                rear_right_motor_enc;

            this->prev_time_ = now;

            this->encoder_invalid_count_ = 0;
        }

        return 0;
    }

    this->encoder_invalid_count_ = 0;


    // ---------------------------------------------------------
    // 5. Valid frame -> update previous encoder
    // ---------------------------------------------------------
    this->front_left_motor_prev_enc_ =
        front_left_motor_enc;

    this->front_right_motor_prev_enc_ =
        front_right_motor_enc;

    this->rear_left_motor_prev_enc_ =
        rear_left_motor_enc;

    this->rear_right_motor_prev_enc_ =
        rear_right_motor_enc;


    // ---------------------------------------------------------
    // 6. Encoder count -> wheel angle delta [rad]
    // ---------------------------------------------------------
    constexpr double ENC_TO_RAD =
        (2.0 * PI)
        / static_cast<double>(MOTOR_ENC_CNT);

    const double front_left_motor_delta_theta =
        static_cast<double>(front_left_delta_enc)
        * ENC_TO_RAD
        * this->is_motor_reversed_;

    const double front_right_motor_delta_theta =
        static_cast<double>(front_right_delta_enc)
        * ENC_TO_RAD
        * this->is_motor_reversed_;

    const double rear_left_motor_delta_theta =
        static_cast<double>(rear_left_delta_enc)
        * ENC_TO_RAD
        * this->is_motor_reversed_;

    const double rear_right_motor_delta_theta =
        static_cast<double>(rear_right_delta_enc)
        * ENC_TO_RAD
        * this->is_motor_reversed_;


    // ---------------------------------------------------------
    // 7. Mecanum forward kinematics
    //
    // wheel_radius_     : mm
    // wheel_base_       : mm
    // wheel_seperate_   : mm
    //
    // dtx, dty          : m
    // dtz               : rad
    // ---------------------------------------------------------
    const double wheel_radius_m =
        this->wheel_radius_ / 1000.0;

    const double rotation_radius =
        0.5
        * (this->wheel_base_
           + this->wheel_seperate_);

    const double dtx =
        (wheel_radius_m / 4.0)
        * (
            front_left_motor_delta_theta
            + front_right_motor_delta_theta
            + rear_left_motor_delta_theta
            + rear_right_motor_delta_theta
        );

    const double dty =
        (wheel_radius_m / 4.0)
        * (
            -front_left_motor_delta_theta
            + front_right_motor_delta_theta
            + rear_left_motor_delta_theta
            - rear_right_motor_delta_theta
        );

    const double dtz =
        (this->wheel_radius_
         / (4.0 * rotation_radius))
        * (
            -front_left_motor_delta_theta
            + front_right_motor_delta_theta
            - rear_left_motor_delta_theta
            + rear_right_motor_delta_theta
        );


    // ---------------------------------------------------------
    // 8. Body displacement -> odom/world frame
    // ---------------------------------------------------------
    const double yaw =
        this->w_z_;

    this->x_pos_ +=
        dtx * std::cos(yaw)
        - dty * std::sin(yaw);

    this->y_pos_ +=
        dtx * std::sin(yaw)
        + dty * std::cos(yaw);

    this->w_z_ += dtz;


    // ---------------------------------------------------------
    // 9. Normalize yaw [-pi, pi]
    // ---------------------------------------------------------
    if(this->w_z_ >= PI)
    {
        this->w_z_ -= 2.0 * PI;
    }
    else if(this->w_z_ <= -PI)
    {
        this->w_z_ += 2.0 * PI;
    }


    // ---------------------------------------------------------
    // 10. Body velocity
    // ---------------------------------------------------------
    const double vbx =
        dtx / dt_sec;

    const double vby =
        dty / dt_sec;

    const double wbz =
        dtz / dt_sec;

    this->vbx_ =
        std::isnan(vbx)
        ? 0.0
        : vbx;

    this->vby_ =
        std::isnan(vby)
        ? 0.0
        : vby;

    this->wbz_ =
        std::isnan(wbz)
        ? 0.0
        : wbz;


    // ---------------------------------------------------------
    // 11. Commit sample time and publish
    // ---------------------------------------------------------
    this->prev_time_ = now;

    return PublishOdometry();
}

int MotorElement::PublishOdometry()
{
    if(odometry_sequence_==std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
    data::OdometryData sample{};
    sample.x_m=static_cast<float>(x_pos_);
    sample.y_m=static_cast<float>(y_pos_);
    sample.yaw_rad=static_cast<float>(w_z_);
    sample.linear_x_mps=static_cast<float>(vbx_);
    sample.linear_y_mps=static_cast<float>(vby_);
    sample.angular_z_rad_s=static_cast<float>(wbz_);
    const int timed=common::GetMonotonicTimestampUs(sample.header.timestamp_us);
    if(timed) return timed;
    sample.header.sequence=++odometry_sequence_;
    const int result=odometry_pub_.Publish(sample);
    return result==-EAGAIN ? 0 : result;
}

void MotorElement::Shutdown()
{
    int error=0;
    const auto record=[&](int result) { if(result && !error) error=result; };
    if(command_opened_) { record(command_sub_.Close()); command_opened_=false; }
    if(status_opened_) { record(status_sub_.Close()); status_opened_=false; }
    if(driver_initialized_)
    {
        // Best effort even when Write throws; subsequent cleanup must continue.
        try { record(driver_.Write({MOTOR_SPEEDS,{0,0,0,0}})); }
        catch(...) { record(-EFAULT); }
        record(driver_.Close()); driver_initialized_=false;
    }
    if(odometry_opened_) { record(odometry_pub_.Close()); odometry_opened_=false; }
    if(odometry_owned_) { record(odometry_pub_.Unlink()); odometry_owned_=false; }
    if(error) throw std::runtime_error("Motor cleanup failed: "+std::to_string(error));
}
} // namespace mecanum::motor
