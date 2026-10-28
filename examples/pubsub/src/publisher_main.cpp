#include "pubsub/message.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <time.h>

class PublisherElement : public kcf::ProcessElement
{
public:
    PublisherElement(kcf::ExecutionMode mode, unsigned int frequency_hz)
        : mode_(mode), frequency_hz_(frequency_hz) {}

    int Setup() override
    {
        const int result = publisher_.Create(pubsub::TOPIC);
        owned_ = result == 0;
        if (result == 0)
        {
            std::cout << "[Publisher] mode="
                      << (mode_ == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")
                      << " target_hz=" << frequency_hz_ << std::endl;
        }
        return result;
    }

    int Loop() override
    {
        if (message_.count == std::numeric_limits<std::int32_t>::max()) return -EOVERFLOW;

        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -errno;
        const auto now_us = static_cast<std::uint64_t>(now.tv_sec) * 1000000u
                          + static_cast<std::uint64_t>(now.tv_nsec) / 1000u;

        if (!report_started_)
        {
            report_start_us_ = now_us;
            report_started_ = true;
        }

        // Keep the original message schema and per-attempt sequence behavior.
        message_.timestamp_us = now_us;
        ++message_.sequence;
        ++message_.count;
        message_.value = message_.count * 0.5f;
        message_.active = true;

        const int result = publisher_.Publish(message_);
        ++attempted_in_window_;
        if (result == -EAGAIN)
        {
            // A busy latest-value Topic may skip a sample without stopping the Runtime.
            ++busy_in_window_;
        }
        else if (result != 0)
        {
            return result;
        }
        else
        {
            ++published_in_window_;
            ++published_total_;
            if (frequency_hz_ == 1)
            {
                // Preserve the original 1 Hz example output.
                std::cout << "[Publisher] seq=" << message_.sequence
                          << " count=" << message_.count
                          << " value=" << message_.value << std::endl;
            }
        }

        if (frequency_hz_ != 1 && now_us - report_start_us_ >= 1000000u)
        {
            const double elapsed_s = static_cast<double>(now_us - report_start_us_) / 1000000.0;
            const double actual_hz = static_cast<double>(published_in_window_) / elapsed_s;
            std::cout << std::fixed << std::setprecision(1)
                      << "[Publisher] target_hz=" << frequency_hz_
                      << " publish_hz=" << actual_hz
                      << " attempted=" << attempted_in_window_
                      << " published=" << published_in_window_
                      << " busy=" << busy_in_window_
                      << " seq=" << message_.sequence << std::endl;
            report_start_us_ = now_us;
            attempted_in_window_ = 0;
            published_in_window_ = 0;
            busy_in_window_ = 0;
        }
        return 0;
    }

    void Shutdown() override
    {
        if (!owned_) return;
        const int closed = publisher_.Close();
        const int unlinked = publisher_.Unlink();
        owned_ = false;
        if (closed || unlinked) throw std::runtime_error("Publisher cleanup failed");
        std::cout << "[Publisher] total_published=" << published_total_ << std::endl;
        std::cout << "[Publisher] Shutdown complete" << std::endl;
    }

private:
    kcf::ExecutionMode mode_;
    unsigned int frequency_hz_{1};
    kcf::Publisher<pubsub::PubSubMessage> publisher_;
    pubsub::PubSubMessage message_{};
    bool owned_{false};
    bool report_started_{false};
    std::uint64_t report_start_us_{0};
    std::uint64_t attempted_in_window_{0};
    std::uint64_t published_in_window_{0};
    std::uint64_t busy_in_window_{0};
    std::uint64_t published_total_{0};
};

int main(int argc, char* argv[])
{
    unsigned int frequency_hz = 1;
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "Usage: kcf_pubsub_publisher [--hz N]  (N: 1..1000; default: 1)\n";
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--hz")
    {
        const std::string_view value(argv[2]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frequency_hz);
        if (value.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() ||
            frequency_hz == 0 || frequency_hz > 1000)
        {
            std::cerr << "Invalid --hz value (expected integer 1..1000)\n";
            return 2;
        }
    }
    else if (argc != 1)
    {
        std::cerr << "Usage: kcf_pubsub_publisher [--hz N]  (N: 1..1000; default: 1)\n";
        return 2;
    }

    PublisherElement element(kcf::DetectLaunchExecutionMode(), frequency_hz);
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(static_cast<double>(frequency_hz));
    return runtime.Run(element);
}
