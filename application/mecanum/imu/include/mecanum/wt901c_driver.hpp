#pragma once
#include "mecanum/imu_messages.hpp"
#include "wt901c/wit_c_sdk.h"
#include <array>
#include <atomic>
#include <string>

namespace mecanum::imu {
std::uint64_t NowNs();
struct SensorEvent {
    std::uint64_t timestamp_ns{};
    std::uint32_t component{}; // ACC, GYRO, ANGLE, or 8 (magnetic)
    std::int16_t raw[3]{};
};
class Wt901cDriver {
public:
    ~Wt901cDriver() { Close(); }
    Wt901cDriver()=default;
    Wt901cDriver(const Wt901cDriver&)=delete;
    Wt901cDriver& operator=(const Wt901cDriver&)=delete;
    int Open(const std::string& device, unsigned baud);
    int Close();
    int Poll(); // one lifecycle thread, <=512 bytes, preserves partial SDK frame
    const auto& Events() const { return events_; }
    std::size_t EventCount() const { return count_; }
    struct Diagnostics {
        std::uint64_t bytes{}, budget_hits{}, uart_errors{}, sdk_errors{};
        WitRxStats parser{};
    };
    const Diagnostics& Stats() const { return stats_; }
private:
    static void Updated(std::uint32_t reg, std::uint32_t count);
    // Upstream SDK is process-global: explicitly reject a second active instance.
    static std::atomic<Wt901cDriver*> active_;
    int fd_{-1};
    std::array<SensorEvent,64> events_{};
    std::size_t count_{};
    Diagnostics stats_{};
};
}
