#pragma once
#include "mecanum/ekf/filter.hpp"
#include "mecanum/ekf/message.hpp"
#include "mecanum/ekf/connection.hpp"
#include "mecanum/messages.hpp"
#include "mecanum/imu_messages.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_element.hpp"
#include <vector>

namespace mecanum::ekf {
std::uint64_t NowNs();
struct Options : Selection {
    std::string wheel_topic{"/mecanum/wheel_odometry"},imu_topic{"/mecanum/imu/raw_data"};
    std::string output_topic{"/mecanum/ekf/odometry"};
    std::uint64_t timeout_ns{500000000ULL}; // trial freshness, not motor command timeout
    Noise noise{};
};
class Element final : public kcf::ProcessElement {
public:
    explicit Element(Options options={});
    ~Element() override { Cleanup(); }
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
    const EstimateMessage& LastOutput() const { return output_; }
private:
    friend struct EkfTestAccess;
    int Cleanup();
    void Connections();
    bool NewPacket(bool imu,std::uint64_t sequence,std::uint64_t timestamp);
    Options options_;
    Fusion fusion_;
    Connection<WheelOdometry> wheel_;
    Connection<imu::ImuSample> imu_;
    kcf::Publisher<EstimateMessage> publisher_;
    EstimateMessage output_{};
    bool owned_{};
    std::uint64_t wheel_transport_{}, wheel_sequence_{}, imu_sequence_{};
    std::uint64_t wheel_packet_time_{}, imu_packet_time_{};
    std::uint64_t wheel_generation_{}, imu_generation_{}, logical_wheel_{}, logical_imu_{};
    std::uint32_t axis_mask_{};
    bool have_axis_mask_{};
    std::uint64_t packet_duplicates_{}, bad_packets_{};
};
}
