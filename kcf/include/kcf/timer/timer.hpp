#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace kcf
{

// Create starts one worker; the first callback runs after one period.
// Callback runs without internal locks, independently of Runtime/Subscriber.
// Serialize Create, external Stop and destruction on a controlling thread.
// A callback may request Stop, but cannot join itself: an external Stop or
// destructor must subsequently join. Do not destroy Timer inside its callback.
// Callbacks must return; Stop waits for an in-flight callback to finish.
class Timer
{
public:
    Timer() = default;
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    int Create(double frequency_hz, std::function<void()> callback);
    void Stop();
    bool IsRunning() const;
    double GetFrequency() const;

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::thread::id worker_id_;
    bool running_{false};
    double frequency_hz_{0.0};
    std::chrono::steady_clock::duration period_{};
    std::function<void()> callback_;
};

} // namespace kcf
