#include "mecanum/imu_element.hpp"
#include <cerrno>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace mecanum::imu {
int ImuElement::Setup() {
    if (ready_) return -EBUSY;
    if (options_.device.empty() || options_.topic_prefix.size()<2 ||
        options_.topic_prefix.front()!='/' || options_.topic_prefix.back()=='/') return -EINVAL;
    int result=driver_.Open(options_.device,options_.baudrate);
    if (result) return result;
    stats_={}; sample_={}; shutdown_error_=0;
    for (unsigned i=0;i<3;++i) if (options_.inverted[i]) sample_.axis_inversion_mask |= 1u<<i;
    result=imu_.Create(options_.topic_prefix+"/imu/raw_data",16);
    if (!result) { imu_owned_=true; result=mag_.Create(options_.topic_prefix+"/imu/mag_raw",16); }
    if (result) { Cleanup(); return result; }
    mag_owned_=true; ready_=true;
    std::cout << "IMU UART ready; awaiting validated sensor frames; depth=16\n";
    return 0;
}
int ImuElement::Loop() {
    if (!ready_) return -ENODEV;
    const int read_result=driver_.Poll();
    // Valid frames preceding a read error are still real samples, not fabricated
    // fault measurements. Publish them, then return the UART/SDK error.
    int publish_error=0;
    constexpr double pi=3.14159265358979323846;
    for (std::size_t n=0;n<driver_.EventCount();++n) {
        const auto& event=driver_.Events()[n];
        int result;
        if (event.component==8) {
            MagSample message{};
            message.sequence=++stats_.mag_samples;
            message.timestamp_ns=event.timestamp_ns;
            message.valid=1; message.axis_inversion_mask=sample_.axis_inversion_mask;
            for (unsigned i=0;i<3;++i)
                message.magnetic_raw_counts[i]=(options_.inverted[i] ? -1 : 1)*std::int32_t(event.raw[i]);
            result=mag_.Publish(message);
            if (!result) ++stats_.mag_published; else ++stats_.mag_publish_failures;
        } else {
            sample_.sequence=++stats_.imu_samples;
            sample_.timestamp_ns=event.timestamp_ns;
            sample_.updated_mask=event.component;
            sample_.valid_mask |= event.component;
            const unsigned component=event.component==ACC ? 0 : event.component==GYRO ? 1 : 2;
            sample_.component_timestamp_ns[component]=event.timestamp_ns;
            for (unsigned i=0;i<3;++i) {
                const double raw=(options_.inverted[i] ? -1.0 : 1.0)*event.raw[i]/32768.0;
                if (event.component==ACC) sample_.acceleration_m_s2[i]=raw*16.0*9.80665;
                else if (event.component==GYRO) sample_.angular_velocity_rad_s[i]=raw*2000.0*pi/180.0;
                else sample_.rpy_rad[i]=raw*pi; // 180 degrees -> pi radians
            }
            if (event.component==ANGLE) {
                const double cr=std::cos(sample_.rpy_rad[0]/2), sr=std::sin(sample_.rpy_rad[0]/2);
                const double cp=std::cos(sample_.rpy_rad[1]/2), sp=std::sin(sample_.rpy_rad[1]/2);
                const double cy=std::cos(sample_.rpy_rad[2]/2), sy=std::sin(sample_.rpy_rad[2]/2);
                sample_.orientation_xyzw[0]=sr*cp*cy-cr*sp*sy;
                sample_.orientation_xyzw[1]=cr*sp*cy+sr*cp*sy;
                sample_.orientation_xyzw[2]=cr*cp*sy-sr*sp*cy;
                sample_.orientation_xyzw[3]=cr*cp*cy+sr*sp*sy;
            }
            result=imu_.Publish(sample_);
            if (!result) ++stats_.imu_published; else ++stats_.imu_publish_failures;
        }
        if (result) {
            stats_.last_publish_error=result;
            if (result!=-EAGAIN && !publish_error) publish_error=result;
            // No retry with a new timestamp: next real frame advances app sequence.
        }
    }
    return read_result ? read_result : publish_error;
}
int ImuElement::Cleanup() {
    ready_=false;
    int error=driver_.Close();
    auto remember=[&](int result) { if (result && !error) error=result; };
    if (imu_owned_) { remember(imu_.Close()); remember(imu_.Unlink()); imu_owned_=false; }
    if (mag_owned_) { remember(mag_.Close()); remember(mag_.Unlink()); mag_owned_=false; }
    return error;
}
void ImuElement::Shutdown() {
    const bool was_ready=ready_;
    const int result=Cleanup();
    if (result) shutdown_error_=result;
    if (was_ready) {
        const auto& d=driver_.Stats();
        std::cout << "IMU samples/published=" << stats_.imu_samples << '/' << stats_.imu_published
                  << " mag=" << stats_.mag_samples << '/' << stats_.mag_published
                  << " publish_failures=" << stats_.imu_publish_failures+stats_.mag_publish_failures
                  << " checksum_errors=" << d.parser.checksum_errors << " discarded_bytes=" << d.parser.discarded_bytes
                  << " unsupported_frames=" << d.parser.unsupported_frames
                  << " sdk_errors=" << d.sdk_errors << " uart_errors=" << d.uart_errors
                  << " budget_hits=" << d.budget_hits << '\n';
    }
    if (result) throw std::runtime_error("IMU resource cleanup failed");
}
}
