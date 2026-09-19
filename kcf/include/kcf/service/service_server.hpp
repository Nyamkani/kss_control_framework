#pragma once

#include "kcf/service/service_protocol.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/detail/service_registry.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include <algorithm>
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
        return Register<Request, Response>(service_id, "", std::move(callback));
    }

    template <typename Request, typename Response>
    int Register(std::uint16_t service_id, const std::string& service_name,
                 std::function<void(const Request&, Response&)> callback)
    {
        static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Response>);
        static_assert(!std::is_pointer_v<Request> && !std::is_pointer_v<Response>);
        static_assert(sizeof(Request) <= SERVICE_MAX_PAYLOAD && sizeof(Response) <= SERVICE_MAX_PAYLOAD);
        if (!callback) return -EINVAL;
        Handler handler{};
        handler.request_size = sizeof(Request); handler.response_size = sizeof(Response);
        handler.request_identity = detail::DeclaredStorageIdentity<Request>();
        handler.response_identity = detail::DeclaredStorageIdentity<Response>();
        handler.register_request = &detail::RegisterEndpointType<Request>;
        handler.register_response = &detail::RegisterEndpointType<Response>;
        handler.info.service_id = service_id;
        handler.info.request_size = sizeof(Request); handler.info.response_size = sizeof(Response);
        // Display metadata failures must never change registration/communication.
        if (service_name.size() < sizeof(handler.info.name) && service_name.find('\0') == std::string::npos)
            std::memcpy(handler.info.name, service_name.data(), service_name.size());
        const auto copy = [](char* target, const char* source, std::size_t size) {
            std::memcpy(target, source, std::min(std::strlen(source), size - 1));
        };
        copy(handler.info.diagnostic_request_type_name, detail::DiagnosticTypeName<Request>(), sizeof(handler.info.diagnostic_request_type_name));
        copy(handler.info.diagnostic_response_type_name, detail::DiagnosticTypeName<Response>(), sizeof(handler.info.diagnostic_response_type_name));
        handler.invoke = [callback = std::move(callback)](const unsigned char* input, unsigned char* output)
        {
            Request request{}; Response response{};
            std::memcpy(&request, input, sizeof(Request));
            callback(request, response);
            std::memcpy(output, &response, sizeof(Response));
        };
        return RegisterHandler(service_id, std::move(handler));
    }

    int Start();
    void Stop(); // poll <=50ms, then join current callback and close socket

private:
    struct Handler
    {
        std::size_t request_size, response_size;
        detail::StorageIdentity request_identity, response_identity;
        ServiceInfo info{};
        std::uint64_t (*register_request)() noexcept;
        std::uint64_t (*register_response)() noexcept;
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
    void PublishServices() noexcept;
    void RemoveServices() noexcept;
    std::uint16_t port_{0};
    int fd_{-1};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::map<std::uint16_t, Handler> handlers_;
    std::array<Cached, 64> cache_{};
    std::size_t next_cache_{0};
};
} // namespace kcf
