#include "kcf/timer/timer.hpp"

#include <cerrno>
#include <cmath>
#include <new>
#include <system_error>
#include <utility>

namespace kcf
{

Timer::~Timer()
{
    Stop();
}

int Timer::Create(double frequency_hz, std::function<void()> callback)
{
    if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0 || !callback)
        return -EINVAL;

    using Clock = std::chrono::steady_clock;
    const auto seconds = std::chrono::duration<long double>(1.0L / frequency_hz);
    if (seconds < Clock::duration{1} || seconds >= Clock::duration::max() / 2)
        return -EINVAL;

    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) return -EBUSY;
    period_ = std::chrono::duration_cast<Clock::duration>(seconds);
    frequency_hz_ = frequency_hz;
    callback_ = std::move(callback);
    running_ = true;
    try
    {
        worker_ = std::thread(&Timer::Run, this);
    }
    catch (const std::system_error& error)
    {
        running_ = false;
        callback_ = {};
        return -error.code().value();
    }
    catch (const std::bad_alloc&)
    {
        running_ = false;
        callback_ = {};
        return -ENOMEM;
    }
    return 0;
}

void Timer::Stop()
{
    bool on_worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        on_worker = worker_id_ == std::this_thread::get_id();
    }
    wake_.notify_all();
    if (!on_worker && worker_.joinable()) worker_.join();
}

bool Timer::IsRunning() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

double Timer::GetFrequency() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frequency_hz_;
}

void Timer::Run()
{
    using Clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lock(mutex_);
    worker_id_ = std::this_thread::get_id();
    auto next_tick = Clock::now() + period_;
    while (running_)
    {
        if (wake_.wait_until(lock, next_tick, [this] { return !running_; })) break;
        lock.unlock();
        try
        {
            callback_();
        }
        catch (...)
        {
            // An application exception stops this worker instead of escaping
            // the thread and terminating the process. IsRunning becomes false.
            lock.lock();
            running_ = false;
            break;
        }
        lock.lock();
        next_tick += period_;
        const auto now = Clock::now();
        if (next_tick <= now) next_tick = now + period_;
    }
    worker_id_ = {};
}

} // namespace kcf
