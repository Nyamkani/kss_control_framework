#pragma once
#include <array>
#include <cstdint>

namespace mecanum::ekf {
using State=std::array<double,6>; // local x,y,yaw; body vx,vy,wz
using Matrix=std::array<double,36>; // row major
struct Noise {
    State q{0.05,0.05,0.06,0.025,0.025,0.02}; // trial variance rates / second
    State p0{1e-6,1e-6,0.04,1,1,1};
    std::array<double,3> wheel_r{0.04,0.04,0.02};
    double gyro_r{0.01}, yaw_r{0.04}; // trial variances, not sensor specifications
};
struct Selection {
    bool use_wheel_vx=true, use_wheel_vy=true, use_wheel_wz=true;
    bool use_imu_yaw=true, use_imu_gyro_z=true;
    bool imu_yaw_relative=true;
    bool Wheel() const { return use_wheel_vx || use_wheel_vy || use_wheel_wz; }
    bool Imu() const { return use_imu_yaw || use_imu_gyro_z; }
};
double Wrap(double angle);
class Filter {
public:
    explicit Filter(Noise noise={});
    static State Transition(const State&,double dt);
    static Matrix Jacobian(const State&,double dt);
    bool Predict(double dt); // finite dt in [0,1] seconds; subdivides at 50 ms
    bool Update(unsigned component,double value,double variance,bool angular=false);
    void StopMotion(); // policy reset: remove stale twist and its cross covariance
    bool Healthy() const;
    const State& X() const { return x_; }
    const Matrix& P() const { return p_; }
    const Noise& Parameters() const { return noise_; }
private:
    friend class Fusion;
    bool InitializeYaw(double yaw);
    Noise noise_;
    State x_{};
    Matrix p_{};
};
enum class Kind { Wheel, Gyro, Yaw };
struct Measurement {
    Kind kind;
    std::uint64_t timestamp_ns;
    std::array<double,3> value{};
};
class Fusion {
public:
    explicit Fusion(std::uint64_t timeout_ns=500000000ULL,Noise noise={},Selection selection={});
    bool Process(const Measurement&,std::uint64_t now); // false: rejected, or unhealthy
    void Expire(std::uint64_t now);
    void SourceChanged(bool imu);
    void InvalidateWheel();
    bool Valid(std::uint64_t now) const;
    bool Suspended() const { return suspended_; }
    const Filter& Estimate() const { return filter_; }
    std::uint64_t Time() const { return time_; }
    std::array<std::uint64_t,3> InputTimes() const { return last_; }
    struct Counters { std::uint64_t accepted{}, duplicates{}, late{}, invalid_time{}, invalid{}, gaps{}, numerical_errors{}; };
    const Counters& Stats() const { return stats_; }
private:
    bool Fresh(std::uint64_t now) const;
    void Suspend(bool new_epoch=false);
    Filter filter_;
    Selection selection_;
    std::uint64_t timeout_, time_{};
    std::array<std::uint64_t,3> last_{}; // wheel, gyro, yaw
    std::array<bool,2> velocity_acquired_{}; // accepted since the last velocity reset
    bool anchored_{false}, suspended_{true};
    double yaw_offset_{};
    Counters stats_{};
};
}
