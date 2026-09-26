#include "mecanum/ekf/element.hpp"
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <time.h>

namespace mecanum::ekf {
std::uint64_t NowNs() {
    timespec t{}; if (clock_gettime(CLOCK_MONOTONIC,&t)) throw std::runtime_error("monotonic clock failed");
    return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
Element::Element(Options o):options_(std::move(o)),fusion_(options_.timeout_ns,options_.noise,options_),
    wheel_(options_.wheel_topic),imu_(options_.imu_topic) {}
int Element::Setup() {
    if (owned_) return -EBUSY;
    const int result=publisher_.Create(options_.output_topic,1);
    if (result) return result;
    owned_=true; output_.session_id=NowNs(); Connections(); return 0;
}
void Element::Connections() {
    const bool wheel_change=options_.Wheel() && wheel_.Refresh(1),imu_change=options_.Imu() && imu_.Refresh(16);
    if (wheel_change) fusion_.SourceChanged(false);
    if (imu_change) fusion_.SourceChanged(true);
    if (wheel_generation_!=wheel_.generation) {
        wheel_generation_=wheel_.generation; ++logical_wheel_;
        wheel_sequence_=wheel_transport_=wheel_packet_time_=0;
    }
    if (imu_generation_!=imu_.generation) {
        imu_generation_=imu_.generation; ++logical_imu_;
        imu_sequence_=imu_packet_time_=0; have_axis_mask_=false;
    }
}
bool Element::NewPacket(bool imu,std::uint64_t sequence,std::uint64_t timestamp) {
    auto& previous=imu ? imu_sequence_ : wheel_sequence_;
    auto& time=imu ? imu_packet_time_ : wheel_packet_time_;
    if (!sequence || !timestamp) { ++bad_packets_; return false; }
    if (sequence==previous || (sequence<previous && timestamp<=time)) { ++packet_duplicates_; return false; }
    if (sequence<previous) {
        // App sequence restarted on a live mapping. Separate logical source epoch.
        fusion_.SourceChanged(imu);
        if (imu) { ++logical_imu_; have_axis_mask_=false; } else ++logical_wheel_;
    } else if (previous && sequence>previous+1) output_.app_sequence_gaps+=sequence-previous-1;
    previous=sequence; time=timestamp; return true;
}
int Element::Loop() {
    if (!owned_) return -ENODEV;
    Connections();
    const auto wg=wheel_.generation,ig=imu_.generation;
    std::vector<Measurement> events; events.reserve(65);
    if (wheel_.connected) {
        WheelOdometry m{}; kcf::TopicReadInfo info{};
        const int result=wheel_.subscriber.ReadLatest(m,info);
        if (!result && info.sequence!=wheel_transport_) {
            if (wheel_transport_ && info.sequence>wheel_transport_+1) output_.wheel_skipped+=info.sequence-wheel_transport_-1;
            wheel_transport_=info.sequence;
            if (NewPacket(false,m.sequence,m.timestamp_ns)) {
                if (!m.valid || m.resynchronized) fusion_.InvalidateWheel();
                else events.push_back({Kind::Wheel,m.timestamp_ns,{m.vx_m_s,m.vy_m_s,m.wz_rad_s}});
            }
        } else if (result && result!=-EAGAIN) { wheel_.Disconnect(); fusion_.SourceChanged(false); }
    }
    if (imu_.connected) for (unsigned n=0;n<32;++n) {
        imu::ImuSample m{}; kcf::TopicReadInfo info{};
        const int result=imu_.subscriber.ReadNext(m,info);
        if (result==-EAGAIN) break;
        if (result) { imu_.Disconnect(); fusion_.SourceChanged(true); break; }
        output_.imu_missed+=info.missed;
        const auto logical_before=logical_imu_;
        if (!NewPacket(true,m.sequence,m.timestamp_ns)) continue;
        if (logical_before!=logical_imu_)
            events.erase(std::remove_if(events.begin(),events.end(),[](const auto& e){return e.kind!=Kind::Wheel;}),events.end());
        if (have_axis_mask_ && axis_mask_!=m.axis_inversion_mask) {
            fusion_.SourceChanged(true); ++logical_imu_;
            // Do not mix earlier frames with a changed axis convention in this batch.
            events.erase(std::remove_if(events.begin(),events.end(),[](const auto& e){return e.kind!=Kind::Wheel;}),events.end());
        }
        have_axis_mask_=true; axis_mask_=m.axis_inversion_mask;
        for (auto component:{imu::GYRO,imu::ANGLE}) {
            const unsigned index=component==imu::GYRO ? 1 : 2;
            if (component==imu::GYRO ? !options_.use_imu_gyro_z : !options_.use_imu_yaw) continue;
            if (!(m.updated_mask&component)) continue;
            if (!(m.valid_mask&component) || m.component_timestamp_ns[index]!=m.timestamp_ns) {
                ++bad_packets_; continue;
            }
            events.push_back({component==imu::GYRO ? Kind::Gyro : Kind::Yaw,m.component_timestamp_ns[index],
                              {component==imu::GYRO ? m.angular_velocity_rad_s[2] : m.rpy_rad[2],0,0}});
        }
    }
    // If the name was replaced during reads, discard that source's staged data.
    Connections();
    events.erase(std::remove_if(events.begin(),events.end(),[&](const auto& e) {
        return e.kind==Kind::Wheel ? (!wheel_.connected || wg!=wheel_.generation) : (!imu_.connected || ig!=imu_.generation);
    }),events.end());
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.timestamp_ns<b.timestamp_ns;});
    const auto now=NowNs();
    for (const auto& event:events) fusion_.Process(event,now);
    fusion_.Expire(now);
    const auto& x=fusion_.Estimate().X(); const auto& p=fusion_.Estimate().P();
    ++output_.sequence; output_.timestamp_ns=fusion_.Time(); output_.published_at_ns=now;
    output_.x_m=x[0]; output_.y_m=x[1]; output_.yaw_rad=x[2];
    output_.vx_m_s=x[3]; output_.vy_m_s=x[4]; output_.wz_rad_s=x[5];
    std::copy(p.begin(),p.end(),output_.covariance);
    const auto times=fusion_.InputTimes(); std::copy(times.begin(),times.end(),output_.input_timestamp_ns);
    output_.wheel_connected=wheel_.connected; output_.imu_connected=imu_.connected;
    output_.wheel_connection_error=wheel_.last_error; output_.imu_connection_error=imu_.last_error;
    output_.valid=(!options_.Wheel() || wheel_.connected) && (!options_.Imu() || imu_.connected) && fusion_.Valid(now);
    output_.wheel_generation=logical_wheel_; output_.imu_generation=logical_imu_;
    output_.reconnects=(wheel_.connections ? wheel_.connections-1 : 0)+(imu_.connections ? imu_.connections-1 : 0);
    const auto& stats=fusion_.Stats();
    output_.discontinuities=stats.gaps; output_.numerical_errors=stats.numerical_errors;
    output_.rejected_time=stats.late+stats.invalid_time;
    output_.duplicates=packet_duplicates_+stats.duplicates;
    output_.rejected_data=bad_packets_+stats.invalid;
    const int result=publisher_.Publish(output_);
    if (result) ++output_.publish_failures;
    if (stats.numerical_errors) return -ERANGE;
    return result==-EAGAIN ? 0 : result;
}
int Element::Cleanup() {
    wheel_.Disconnect(); imu_.Disconnect();
    if (!owned_) return 0;
    const int a=publisher_.Close(),b=publisher_.Unlink(); owned_=false;
    return a ? a : b;
}
void Element::Shutdown() { if (Cleanup()) throw std::runtime_error("EKF cleanup failed"); }
}
