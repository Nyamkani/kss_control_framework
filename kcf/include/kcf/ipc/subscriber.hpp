#pragma once

#include <atomic>
#include <functional>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include "kcf/ipc/shared_channel.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"

namespace kcf
{

// Callback runs on this worker, potentially concurrently with Element::Loop.
// It receives a local copy with NO KCF locks or slot pins held. User state needs
// its own synchronization. Callback must return for Close to join successfully.
// Create/Close/destruction are serialized by the caller on a controlling thread;
// destroying the Subscriber inside its callback is not supported.
template <typename T>
class Subscriber
{
public:
    Subscriber() = default;
    ~Subscriber() { Close(); }
    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    int Create(const std::string& name, std::function<void(const T&)> callback)
    {
        if (worker_.joinable()) return -EBUSY;
        if (!callback) return -EINVAL;
        const int result = channel_.Open(name);
        if (result != 0) return result;
        running_.store(true);
        error_.store(0);
        try
        {
            worker_ = std::thread([this, callback = std::move(callback)]
            {
                try
                {
                    T snapshot {};
                    std::uint32_t last_notify = 0, last_publish = 0;
                    bool delivered = false;
                    while (running_.load())
                    {
                        int result = channel_.Wait(last_notify);
                        if (result == -ECANCELED) break;
                        if (result != 0) { error_.store(result); break; }
                        std::uint32_t sequence = 0;
                        result = channel_.ReadLatestSnapshot(snapshot, sequence);
                        if (result == -ECANCELED) break;
                        if (result == -EAGAIN) continue;
                        if (result != 0) { error_.store(result); break; }
                        if (!running_.load()) break;
                        if (!delivered || sequence != last_publish)
                        {
                            callback(snapshot);
                            last_publish = sequence;
                            delivered = true;
                        }
                    }
                }
                catch (...) { error_.store(-ECANCELED); }
                running_.store(false);
            });
        }
        catch (const std::system_error& error)
        {
            running_.store(false);
            channel_.Close();
            return -error.code().value();
        }
        catch (const std::bad_alloc&)
        {
            running_.store(false);
            channel_.Close();
            return -ENOMEM;
        }
        registration_id_ = detail::RegisterEndpoint(EndpointKind::TOPIC,
            EndpointRole::SUBSCRIBER, name, sizeof(T), detail::DiagnosticTypeName<T>(), detail::RegisterEndpointType<T>());
        return 0;
    }

    int Close()
    {
        if (worker_.joinable())
        {
            if (worker_.get_id() == std::this_thread::get_id()) return -EDEADLK;
            running_.store(false);
            const int result = channel_.StopWait();
            if (result != 0 && result != -ENOTRECOVERABLE) return result;
            worker_.join();
            if (result != 0) error_.store(result);
        }
        const int result = channel_.Close();
        detail::UnregisterEndpoint(registration_id_); registration_id_ = 0;
        return result != 0 ? result : error_.load();
    }

private:
    SharedChannel<T> channel_;
    std::uint64_t registration_id_{0};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> error_{0};
};

} // namespace kcf
