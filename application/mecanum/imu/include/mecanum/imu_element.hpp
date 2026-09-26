#pragma once
#include "mecanum/wt901c_driver.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_element.hpp"
#include <string>

namespace mecanum::imu {
struct ImuOptions {
    std::string device{"/dev/ttyUSB0"}, topic_prefix{"/mecanum"};
    unsigned baudrate{115200};
    bool inverted[3]{false,false,false}; // X=0, Y=1, Z=2, all reported components
};
class ImuElement final : public kcf::ProcessElement {
public:
    explicit ImuElement(ImuOptions options) : options_(std::move(options)) {}
    ~ImuElement() override { Cleanup(); }
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
    struct PublishStats {
        std::uint64_t imu_samples{}, mag_samples{}, imu_published{}, mag_published{};
        std::uint64_t imu_publish_failures{}, mag_publish_failures{};
        int last_publish_error{};
    };
    const PublishStats& Stats() const { return stats_; }
    const Wt901cDriver::Diagnostics& DriverStats() const { return driver_.Stats(); }
    int ShutdownError() const { return shutdown_error_; }
private:
    friend struct ImuTestAccess;
    int Cleanup();
    ImuOptions options_;
    Wt901cDriver driver_;
    kcf::Publisher<ImuSample> imu_;
    kcf::Publisher<MagSample> mag_;
    ImuSample sample_{};
    PublishStats stats_{};
    bool imu_owned_{false}, mag_owned_{false}, ready_{false};
    int shutdown_error_{};
};
}
