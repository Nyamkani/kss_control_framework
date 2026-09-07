#include "kcf/ipc/subscriber.hpp"
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
    unsigned delay_ms = 0, log_every = 1;
    if (argc > 3) return 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view input(argv[i]);
        unsigned& value = i == 1 ? delay_ms : log_every;
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
            (i == 1 && value > 5000) || (i == 2 && value == 0))
        {
            std::cerr << "Usage: " << argv[0] << " [callback_delay_ms: 0..5000] [log_every: >0]" << std::endl;
            return 1;
        }
    }
    struct sigaction action {};
    action.sa_handler = HandleStop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) return 1;

    std::uint64_t received = 0, invalid = 0, last = 0, skipped = 0;
    const auto main_thread = std::this_thread::get_id();
    kcf::Subscriber<TestMessage> subscriber;
    const int created = subscriber.Create("/kcf_test_topic", [&](const TestMessage& msg)
    {
        if (msg.count == 0 || msg.count <= last || msg.value != msg.count * 0.5 ||
            std::this_thread::get_id() == main_thread) ++invalid;
        if (last != 0 && msg.count > last) skipped += msg.count - last - 1;
        last = msg.count;
        ++received;
        if (received % log_every == 0)
            std::cout << "[TopicSub] count=" << msg.count << " value=" << msg.value
                      << " worker=1" << std::endl;
        if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });
    if (created != 0)
    {
        std::cerr << "[TopicSub] Create failed: " << created << std::endl;
        return 1;
    }
    // Signal polling is only on main; the worker blocks on notify_cond.
    while (!stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int closed = subscriber.Close(); // join before reading callback counters
    std::cout << "[TopicSub] received=" << received << " last=" << last
              << " skipped=" << skipped << " invalid=" << invalid << std::endl;
    if (closed != 0 || invalid != 0) return 1;
    std::cout << "[TopicSub] Shutdown complete" << std::endl;
    return 0;
}
