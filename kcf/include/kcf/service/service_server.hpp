#pragma once

#include "kcf/service/service_protocol.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <map>
#include <thread>
#include <utility>

namespace kcf
{
// One endpoint per Element process. Create/Register/Start/Stop are serialized
// by its controlling thread. Callbacks run sequentially, with no internal lock,
// on the worker; keep them short and do not destroy the server from a callback.
class ServiceServer
{
public:
    ServiceServer() = default;
    ~ServiceServer();
    ServiceServer(const ServiceServer&) = delete;
    ServiceServer& operator=(const ServiceServer&) = delete;
    int Create(std::uint16_t port);

    template <typename Request, typename Response>
    int Register(std::uint16_t service_id, std::function<void(const Request&, Response&)> callback)
    {
        static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Response>);
        static_assert(!std::is_pointer_v<Request> && !std::is_pointer_v<Response>);
        static_assert(sizeof(Request) <= SERVICE_MAX_PAYLOAD && sizeof(Response) <= SERVICE_MAX_PAYLOAD);
        if (!callback) return -EINVAL;
        return RegisterHandler(service_id, {sizeof(Request), sizeof(Response),
            [callback = std::move(callback)](const unsigned char* input, unsigned char* output)
            {
                Request request{};
                Response response{};
                std::memcpy(&request, input, sizeof(Request));
                callback(request, response);
                std::memcpy(output, &response, sizeof(Response));
            }});
    }

    int Start();
    void Stop(); // poll <=50ms, then join current callback and close socket

private:
    struct Handler
    {
        std::size_t request_size, response_size;
        std::function<void(const unsigned char*, unsigned char*)> invoke;
    };
    struct Cached
    {
        bool valid{false};
        std::uint32_t address{0};
        std::uint16_t port{0};
        detail::ServicePacket request;
        detail::ServicePacket response;
    };
    int RegisterHandler(std::uint16_t service_id, Handler handler);
    void Run();
    int fd_{-1};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::map<std::uint16_t, Handler> handlers_;
    std::array<Cached, 64> cache_{};
    std::size_t next_cache_{0};
};
} // namespace kcf
