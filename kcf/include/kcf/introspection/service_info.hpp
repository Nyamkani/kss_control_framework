#pragma once
#include <cstdint>
#include <type_traits>
namespace kcf {
struct ServiceInfo {
    std::uint64_t registration_id{0};
    char name[241]{};
    std::uint8_t reserved0{0};
    std::uint16_t port{0}, service_id{0};
    std::uint16_t reserved1{0};
    std::uint32_t request_size{0}, response_size{0};
    std::uint64_t request_type_id{0}, response_type_id{0};
    char diagnostic_request_type_name[160]{};
    char diagnostic_response_type_name[160]{};
};
static_assert(std::is_trivially_copyable_v<ServiceInfo>);
static_assert(sizeof(ServiceInfo)==600);
}
