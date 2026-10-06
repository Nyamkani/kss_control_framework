#include "kcf/ipc/publisher.hpp"
#include "topic/test_message.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <signal.h>
#include <string_view>
#include <thread>

namespace
{
volatile sig_atomic_t stop_requested = 0;
void HandleStop(int) { stop_requested = 1; }
}

int main(int argc, char* argv[])
{
    unsigned hz = 2;
    if (argc > 2) return 1;
    if (argc == 2)
    {
        const std::string_view input(argv[1]);
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), hz);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
            hz == 0 || hz > 1000)
        {
            std::cerr << "Usage: " << argv[0] << " [hz: 1..1000]" << std::endl;
            return 1;
        }
    }
    struct sigaction action {};
    action.sa_handler = HandleStop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) return 1;

    kcf::Publisher<TestMessage> publisher;
    const int created = publisher.Create("/kcf_test_topic");
    if (created != 0)
    {
        std::cerr << "[TopicPub] Create failed: " << created << std::endl;
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000 / hz);
    auto next = Clock::now();
    TestMessage message {};
    std::uint64_t retries = 0;
    int result = 0;
    while (!stop_requested)
    {
        const TestMessage next_message{message.count + 1, (message.count + 1) * 0.5};
        result = publisher.Publish(next_message);
        if (result == -EAGAIN) { ++retries; result = 0; }
        else if (result != 0) break;
        else
        {
            message = next_message;
            if (hz <= 10 || message.count % hz == 0)
                std::cout << "[TopicPub] count=" << message.count << " value=" << message.value << std::endl;
        }
        next += period;
        const auto now = Clock::now();
        if (now < next) std::this_thread::sleep_until(next);
        else next = now;
    }
    const int unlinked = publisher.Unlink();
    const int closed = publisher.Close();
    std::cout << "[TopicPub] total=" << message.count << " retries=" << retries << std::endl;
    if (result != 0 || unlinked != 0 || closed != 0) return 1;
    std::cout << "[TopicPub] Cleanup complete" << std::endl;
    return 0;
}
