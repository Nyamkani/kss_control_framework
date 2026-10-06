#include "kcf/timer/timer.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "topic/test_message.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void Check(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

void PeriodicTest(double hz, Clock::duration duration, std::size_t minimum, std::size_t maximum)
{
    std::vector<Clock::time_point> ticks;
    bool worker_context = true;
    const auto main_thread = std::this_thread::get_id();
    kcf::Timer timer;
    const auto start = Clock::now();
    Check(timer.Create(hz, [&]
    {
        // Getters take the Timer mutex: this also detects lock-held callbacks.
        (void)timer.IsRunning();
        worker_context = worker_context && std::this_thread::get_id() != main_thread
                         && timer.GetFrequency() == hz;
        ticks.push_back(Clock::now());
    }) == 0, "Timer Create failed");
    Check(timer.Create(hz, [] {}) != 0, "Duplicate Create accepted");
    std::this_thread::sleep_for(duration);
    timer.Stop();
    timer.Stop();
    Check(!timer.IsRunning() && worker_context, "Timer threading/state failure");
    Check(ticks.size() >= minimum && ticks.size() <= maximum, "Unexpected callback count");
    const double first = std::chrono::duration<double>(ticks.front() - start).count();
    const double average = std::chrono::duration<double>(ticks.back() - ticks.front()).count() / (ticks.size() - 1);
    Check(first >= 0.8 / hz, "First callback ran before first period");
    Check(std::abs(average - 1.0 / hz) < 0.2 / hz, "Unexpected callback interval");
    std::cout << hz << " Hz: count=" << ticks.size() << " average_ms=" << average * 1000 << std::endl;
}

void StopTest()
{
    std::atomic<unsigned> calls{0};
    kcf::Timer timer;
    Check(timer.Create(0.1, [&] { ++calls; }) == 0, "Long Timer Create failed");
    std::this_thread::sleep_for(1s);
    const auto start = Clock::now();
    timer.Stop();
    const auto elapsed = Clock::now() - start;
    Check(elapsed < 500ms && calls == 0, "Stop waited for long period");
    timer.Stop();
    std::cout << "Stop: " << std::chrono::duration<double, std::milli>(elapsed).count() << " ms" << std::endl;

    std::promise<void> entered;
    bool completed = false;
    Check(timer.Create(100, [&]
    {
        entered.set_value();
        std::this_thread::sleep_for(100ms);
        completed = true;
    }) == 0, "Timer reuse failed");
    Check(entered.get_future().wait_for(1s) == std::future_status::ready, "Callback never entered");
    timer.Stop();
    Check(completed, "Stop did not join active callback");

    Check(timer.Create(100, [&] { timer.Stop(); }) == 0, "Self-stop Create failed");
    std::this_thread::sleep_for(50ms);
    Check(!timer.IsRunning(), "Callback Stop failed");
    timer.Stop();
    {
        kcf::Timer scoped;
        Check(scoped.Create(0.1, [] {}) == 0, "Destructor test Create failed");
    }
}

void OverrunTest()
{
    std::vector<Clock::time_point> starts, ends;
    kcf::Timer timer;
    Check(timer.Create(100, [&]
    {
        starts.push_back(Clock::now());
        std::this_thread::sleep_for(20ms);
        ends.push_back(Clock::now());
    }) == 0, "Overrun Create failed");
    std::this_thread::sleep_for(350ms);
    timer.Stop();
    Check(starts.size() >= 5 && starts.size() <= 13, "Overrun burst/count failure");
    double minimum_gap = 1e9;
    for (std::size_t i = 1; i < starts.size(); ++i)
    {
        const double gap = std::chrono::duration<double, std::milli>(starts[i] - ends[i - 1]).count();
        minimum_gap = std::min(minimum_gap, gap);
        Check(gap >= 8.0, "Overrun catch-up burst detected");
    }
    std::cout << "Overrun: count=" << starts.size() << " minimum_post_callback_gap_ms=" << minimum_gap << std::endl;
}

void TopicTest()
{
    const std::string name = "/kcf_timer_test_" + std::to_string(getpid());
    kcf::Publisher<TestMessage> publisher;
    Check(publisher.Create(name) == 0, "Topic Create failed");
    try
    {
        std::atomic<std::uint64_t> received{0}, last{0}, invalid{0};
        std::uint64_t sent = 0;
        int publish_error = 0;
        kcf::Subscriber<TestMessage> subscriber;
        Check(subscriber.Create(name, [&](const TestMessage& msg)
        {
            if (msg.count <= last.load() || msg.value != msg.count * 0.5) ++invalid;
            last.store(msg.count);
            ++received;
        }) == 0, "Subscriber Create failed");
        kcf::Timer timer;
        Check(timer.Create(100, [&]
        {
            const TestMessage msg{sent + 1, (sent + 1) * 0.5};
            const int result = publisher.Publish(msg);
            if (result == 0) ++sent;
            else if (result != -EAGAIN) publish_error = result;
        }) == 0, "Publishing Timer Create failed");
        std::this_thread::sleep_for(3s);
        timer.Stop(); // stop publishing before closing the Publisher
        const auto deadline = Clock::now() + 1s;
        while (last.load() != sent && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        Check(subscriber.Close() == 0, "Subscriber Close failed");
        Check(sent >= 250 && sent <= 320 && received >= 200 && last == sent && invalid == 0 && publish_error == 0,
              "Timer/Topic integration failed");
        std::cout << "Topic: sent=" << sent << " received=" << received << " last=" << last
                  << " invalid=" << invalid << std::endl;
    }
    catch (...)
    {
        publisher.Unlink();
        publisher.Close();
        throw;
    }
    const int unlinked = publisher.Unlink();
    const int closed = publisher.Close();
    Check(unlinked == 0 && closed == 0, "Topic cleanup failed");
}
}

int main()
{
    try
    {
        kcf::Timer timer;
        for (double hz : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            Check(timer.Create(hz, [] {}) != 0, "Invalid frequency accepted");
        Check(timer.Create(100, {}) != 0, "Empty callback accepted");
        PeriodicTest(2, 5100ms, 9, 11);
        PeriodicTest(100, 3s, 250, 320);
        StopTest();
        OverrunTest();
        TopicTest();
        std::cout << "Timer tests PASS" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Timer tests FAIL: " << error.what() << std::endl;
        return 1;
    }
}
