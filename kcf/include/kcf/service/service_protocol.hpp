#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kcf
{
// Native-endian local IPC only: peers must share KCF version, ABI and types.
constexpr std::uint32_t SERVICE_MAGIC = 0x4b434653;
constexpr std::uint16_t SERVICE_PROTOCOL_VERSION = 1;
constexpr std::size_t SERVICE_MAX_PAYLOAD = 4096;

enum class ServicePacketType : std::uint16_t { REQUEST = 1, RESPONSE = 2 };

struct ServiceHeader
{
    std::uint32_t magic{SERVICE_MAGIC};
    std::uint16_t protocol_version{SERVICE_PROTOCOL_VERSION};
    ServicePacketType packet_type{ServicePacketType::REQUEST};
    std::uint16_t service_id{0};
    std::uint16_t reserved{0};
    std::uint32_t payload_size{0};
    std::uint64_t client_id{0};
    std::uint64_t request_id{0};
    std::int32_t framework_status{0};
    std::uint32_t reserved2{0};
};
static_assert(sizeof(ServiceHeader) == 40);
static_assert(std::is_trivially_copyable_v<ServiceHeader>);

namespace detail
{
struct ServicePacket
{
    ServiceHeader header;
    std::array<unsigned char, SERVICE_MAX_PAYLOAD> payload{};
};
static_assert(offsetof(ServicePacket, payload) == sizeof(ServiceHeader));

inline bool ValidHeader(const ServiceHeader& header, ServicePacketType type)
{
    return header.magic == SERVICE_MAGIC && header.protocol_version == SERVICE_PROTOCOL_VERSION
        && header.packet_type == type && header.reserved == 0 && header.reserved2 == 0
        && header.client_id != 0 && header.request_id != 0;
}
}
} // namespace kcf
