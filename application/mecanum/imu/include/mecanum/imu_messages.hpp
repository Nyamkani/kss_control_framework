#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mecanum::imu {
constexpr std::uint32_t ACC=1, GYRO=2, ANGLE=4;
// imu_link; XYZ/RPY, quaternion XYZW. Times are host CLOCK_MONOTONIC
// frame-validation times, NOT hardware acquisition/synchronization timestamps.
struct ImuSample {
    std::uint64_t sequence{}, timestamp_ns{};
    std::uint64_t component_timestamp_ns[3]{}; // acceleration, gyro, angle
    std::uint32_t updated_mask{}, valid_mask{}, axis_inversion_mask{};
    double acceleration_m_s2[3]{}, angular_velocity_rad_s[3]{}, rpy_rad[3]{};
    double orientation_xyzw[4]{}; // valid only when ANGLE is set
};
struct MagSample {
    std::uint64_t sequence{}, timestamp_ns{};
    std::uint32_t valid{}, axis_inversion_mask{};
    std::int32_t magnetic_raw_counts[3]{}; // no verified Tesla scale/calibration
};
static_assert(std::is_trivially_copyable_v<ImuSample> && std::is_standard_layout_v<ImuSample>);
static_assert(std::is_trivially_copyable_v<MagSample> && std::is_standard_layout_v<MagSample>);
}
namespace kcf {
template<> struct TypeDescriptorTraits<mecanum::imu::ImuSample> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        using T=mecanum::imu::ImuSample;
        auto d=MakeTypeDescriptor<T>("kcf.mecanum.ImuSample"); d.protocol_version=1;
#define FIELD(n) d.fields[d.field_count++]=MakeField<T,decltype(T::n)>(#n,offsetof(T,n))
        FIELD(sequence); FIELD(timestamp_ns); FIELD(component_timestamp_ns);
        FIELD(updated_mask); FIELD(valid_mask); FIELD(axis_inversion_mask);
        FIELD(acceleration_m_s2); FIELD(angular_velocity_rad_s); FIELD(rpy_rad); FIELD(orientation_xyzw);
#undef FIELD
        return d;
    }
};
template<> struct TypeDescriptorTraits<mecanum::imu::MagSample> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        using T=mecanum::imu::MagSample;
        auto d=MakeTypeDescriptor<T>("kcf.mecanum.MagRawSample"); d.protocol_version=1;
#define FIELD(n) d.fields[d.field_count++]=MakeField<T,decltype(T::n)>(#n,offsetof(T,n))
        FIELD(sequence); FIELD(timestamp_ns); FIELD(valid); FIELD(axis_inversion_mask); FIELD(magnetic_raw_counts);
#undef FIELD
        return d;
    }
};
}
