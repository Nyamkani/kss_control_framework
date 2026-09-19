#pragma once

#include "kcf/service/service_protocol.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include <mutex>

namespace kcf
{
class ServiceClient
{
public:
    ServiceClient() = default;
    ~ServiceClient();
    ServiceClient(const ServiceClient&) = delete;
    ServiceClient& operator=(const ServiceClient&) = delete;
    // retry_count excludes the initial attempt: 2 means at most 3 sends.
    int Create(std::uint16_t target_port, std::uint32_t timeout_ms = 200,
               std::uint32_t retry_count = 2);

    template <typename Request, typename Response>
    int Call(std::uint16_t service_id, const Request& request, Response& response)
    {
        static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Response>);
        static_assert(!std::is_pointer_v<Request> && !std::is_pointer_v<Response>);
        static_assert(sizeof(Request) <= SERVICE_MAX_PAYLOAD && sizeof(Response) <= SERVICE_MAX_PAYLOAD);
        // Explicit traits are resolved once per typed Call specialization.
        static const auto request_identity = detail::DeclaredStorageIdentity<Request>();
        static const auto response_identity = detail::DeclaredStorageIdentity<Response>();
        return CallRaw(service_id, &request, sizeof(Request), &response, sizeof(Response),
                       request_identity, response_identity);
    }
    void Close(); // serialized with Call; waits for its bounded retry cycle

private:
    friend class DynamicServiceClient;
    int CallRaw(std::uint16_t service_id, const void* request, std::size_t request_size,
                void* response, std::size_t response_size,
                detail::StorageIdentity request_identity, detail::StorageIdentity response_identity);
    std::mutex mutex_;
    int fd_{-1};
    std::uint16_t target_port_{0};
    std::uint32_t timeout_ms_{200}, retry_count_{2};
    std::uint64_t client_id_{0}, request_id_{0};
};
} // namespace kcf
