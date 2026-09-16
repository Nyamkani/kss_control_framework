#pragma once
#include "kcf/dynamic/dynamic_payload.hpp"
#include "kcf/service/service_client.hpp"
namespace kcf {
// Calls are serialized by the existing transport. Serialize Open/Close with
// local calls. Request/response descriptors are required, as in R4 dynamic IPC.
class DynamicServiceClient {
public:
    int Open(std::uint16_t port,std::uint16_t service_id,
             const TypeDescriptor& request,const TypeDescriptor& response,
             std::uint32_t timeout_ms=200,std::uint32_t retry_count=2);
    int Call(const DynamicPayload& request,DynamicPayload& response);
    void Close();
private:
    ServiceClient client_;
    bool opened_{false};std::uint16_t service_id_{0};
    std::uint32_t request_size_{0},response_size_{0};
    detail::StorageIdentity request_identity_,response_identity_;
};
}
