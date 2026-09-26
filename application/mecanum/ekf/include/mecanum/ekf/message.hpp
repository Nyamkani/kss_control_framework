#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace mecanum::ekf {
struct EstimateMessage {
    std::uint64_t session_id{}, sequence{}, timestamp_ns{}, published_at_ns{};
    std::uint64_t input_timestamp_ns[3]{}; // wheel, gyro, yaw
    double x_m{}, y_m{}, yaw_rad{}, vx_m_s{}, vy_m_s{}, wz_rad_s{};
    double covariance[36]{}; // row-major x,y,yaw,vx,vy,wz
    bool valid{}, wheel_connected{}, imu_connected{};
    std::int32_t wheel_connection_error{}, imu_connection_error{};
    std::uint64_t wheel_generation{}, imu_generation{}, reconnects{}, discontinuities{};
    std::uint64_t imu_missed{}, wheel_skipped{}, app_sequence_gaps{};
    std::uint64_t rejected_time{}, duplicates{}, rejected_data{}, numerical_errors{}, publish_failures{};
};
static_assert(std::is_trivially_copyable_v<EstimateMessage> && std::is_standard_layout_v<EstimateMessage>);
}
namespace kcf {
template<> struct TypeDescriptorTraits<mecanum::ekf::EstimateMessage> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        using T=mecanum::ekf::EstimateMessage;
        auto d=MakeTypeDescriptor<T>("kcf.mecanum.Ekf2D.v1"); d.protocol_version=1;
#define F(n) d.fields[d.field_count++]=MakeField<T,decltype(T::n)>(#n,offsetof(T,n))
        F(session_id); F(sequence); F(timestamp_ns); F(published_at_ns); F(input_timestamp_ns);
        F(x_m); F(y_m); F(yaw_rad); F(vx_m_s); F(vy_m_s); F(wz_rad_s); F(covariance);
        F(valid); F(wheel_connected); F(imu_connected); F(wheel_generation); F(imu_generation);
        F(wheel_connection_error); F(imu_connection_error);
        F(reconnects); F(discontinuities); F(imu_missed); F(wheel_skipped); F(app_sequence_gaps);
        F(rejected_time); F(duplicates); F(rejected_data); F(numerical_errors); F(publish_failures);
#undef F
        return d;
    }
};
}
