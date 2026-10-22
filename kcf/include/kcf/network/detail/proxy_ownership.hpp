#pragma once
#include "kcf/network/types.hpp"
#include <string>
namespace kcf::network {
// Process-local diagnostics; not part of any Local storage or network wire ABI.
enum class ProxyRecoveryState : std::uint8_t {
    NONE, CREATED, LIVE_NETWORK_OWNER, LOCAL_PUBLISHER, UNKNOWN_OWNERSHIP,
    STALE_DETECTED, RECOVERED, VALIDATION_FAILED, CLEANUP_FAILED
};
namespace detail {
struct ProxyOwnership {
    RemoteEndpointIdentity proxy,source;
    std::uint64_t gateway_session{0},source_session{0};
};
struct ProxyRecord {
    ProxyOwnership owner;
    std::string topic;
    TypeIdentity type;
    std::uint64_t size{0},alignment{0},layout{0},device{0},inode{0},length{0};
};
// Network-only bounded, checksummed, explicit big-endian sidecar format v1.
inline constexpr std::size_t PROXY_RECORD_MAX=512;
std::string ProxySidecarName(const std::string& topic);
int EncodeProxyRecord(const ProxyRecord&,std::vector<std::uint8_t>&);
int DecodeProxyRecord(const std::vector<std::uint8_t>&,ProxyRecord&);
enum class OwnerLiveness { LIVE, DEAD, UNKNOWN };
OwnerLiveness ProbeProxyOwner(std::int32_t pid,std::uint64_t start_ticks);
}
}
