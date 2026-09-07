#pragma once

#include "kcf/parameter/shared_parameter.hpp"
#include <atomic>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>

namespace kcf
{
// Optional watcher starts at the registration-time version; initial/current
// state is retrieved using Get, not delivered as a synthetic change callback.
// Callback receives a local snapshot, outside internal locks, on a worker.
// Serialize lifecycle operations on a controlling thread; Get/Set may overlap.
// Stop application Get/Set calls before Close. Callback must return for join,
// and must not destroy this Parameter or call Close from its own worker.
template <typename T>
class Parameter
{
public:
    Parameter() = default;
    ~Parameter() { Close(); }
    Parameter(const Parameter&) = delete;
    Parameter& operator=(const Parameter&) = delete;

    int Create(const std::string& name, const T& initial_value,
               std::function<void(const T&)> callback = {})
    {
        if (worker_.joinable()) return -EBUSY;
        const int result = shared_.Create(name, initial_value);
        if (result != 0) return result;
        const int watching = Watch(std::move(callback));
        if (watching != 0) { shared_.Unlink(); shared_.Close(); }
        return watching;
    }

    int Open(const std::string& name, std::function<void(const T&)> callback = {})
    {
        if (worker_.joinable()) return -EBUSY;
        const int result = shared_.Open(name);
        if (result != 0) return result;
        const int watching = Watch(std::move(callback));
        if (watching != 0) shared_.Close();
        return watching;
    }

    int Get(T& value, std::uint64_t* version = nullptr) { return shared_.Get(value, version); }
    int Set(const T& value) { return shared_.Set(value); }
    int Unlink() { return shared_.Unlink(); }

    int Close()
    {
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id()) return -EDEADLK;
            running_.store(false);
            const int result = shared_.StopWait();
            if (result != 0) return result;
            worker_.join();
        }
        const int result = shared_.Close();
        return result != 0 ? result : error_.load();
    }

private:
    int Watch(std::function<void(const T&)> callback)
    {
        error_.store(0);
        if (!callback) return 0;
        T snapshot{};
        std::uint64_t version = 0;
        const int result = shared_.Get(snapshot, &version);
        if (result != 0) return result;
        running_.store(true);
        try
        {
            worker_ = std::thread([this, version, callback = std::move(callback)]() mutable
            {
                try
                {
                    T value{};
                    while (running_.load())
                    {
                        const int result = shared_.Wait(value, version);
                        if (result == -ECANCELED) break;
                        if (result != 0) { error_.store(result); break; }
                        if (!running_.load()) break;
                        callback(value);
                    }
                }
                catch (...) { error_.store(-ECANCELED); }
                running_.store(false);
            });
        }
        catch (const std::system_error& error)
        {
            running_.store(false);
            return -error.code().value();
        }
        catch (const std::bad_alloc&)
        {
            running_.store(false);
            return -ENOMEM;
        }
        return 0;
    }

    SharedParameter<T> shared_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> error_{0};
};
} // namespace kcf
