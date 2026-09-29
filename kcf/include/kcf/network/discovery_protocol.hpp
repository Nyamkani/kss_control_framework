#pragma once
#include "kcf/network/compatibility.hpp"

namespace kcf::network
{
inline constexpr std::uint32_t DISCOVERY_MAGIC = 0x4b434644; // KCFD
inline constexpr std::uint16_t DISCOVERY_VERSION = 2;
inline constexpr std::size_t DISCOVERY_HEADER_SIZE = 56;
inline constexpr std::size_t DISCOVERY_MAX_DATAGRAM = 1200;
inline constexpr std::size_t DISCOVERY_CHUNK_SIZE = 1024;
inline constexpr std::size_t DISCOVERY_MAX_METADATA = 256 * 1024;
inline constexpr std::size_t DISCOVERY_MAX_ENDPOINTS = 64;
inline constexpr std::size_t DISCOVERY_MAX_PEERS = 64;
enum class DiscoveryMessage : std::uint16_t { ANNOUNCE = 1, METADATA_REQUEST = 2, METADATA_RESPONSE = 3 };
// Process-local representation, never sent using sizeof/memcpy.
struct DiscoveryPacket
{
    DiscoveryMessage kind{DiscoveryMessage::ANNOUNCE};
    HostIdentity host, boot_id;
    std::uint64_t session_id{0};
    std::uint64_t sequence{0}, revision{0}, request_id{0};
    std::uint32_t metadata_size{0}, offset{0};
    std::uint16_t endpoint_count{0};
    PeerProfile profile; // ANNOUNCE only; immutable within a discovery session
    std::vector<std::uint8_t> chunk;
};
int EncodeDiscoveryPacket(const DiscoveryPacket&, std::vector<std::uint8_t>& output);
int DecodeDiscoveryPacket(const std::uint8_t*, std::size_t, DiscoveryPacket& output);
int EncodeDiscoveryMetadata(const std::vector<NetworkEndpointMetadata>&, std::vector<std::uint8_t>& output);
int DecodeDiscoveryMetadata(const std::uint8_t*, std::size_t, std::vector<NetworkEndpointMetadata>& output);
bool ValidDiscoveryMetadata(const std::vector<NetworkEndpointMetadata>&, const HostIdentity&, const HostIdentity& boot_id);
} // namespace kcf::network
