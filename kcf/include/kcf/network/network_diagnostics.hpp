#pragma once
#include "kcf/network/topic_data_plane.hpp"

namespace kcf::network {
// Read-only snapshot codec carried by Control DIAGNOSTICS_RESPONSE.
// ControlNode advertises NETWORK_DIAGNOSTICS only with diagnostics_enabled opt-in.
inline constexpr std::uint16_t NETWORK_DIAGNOSTICS_VERSION = 1;
inline constexpr std::size_t MAX_NETWORK_DIAGNOSTICS_PAYLOAD = 16 * 1024;
struct NetworkDiagnosticsSnapshot {
    HostIdentity gateway_host, gateway_boot_id;
    std::uint64_t gateway_session{0};
    std::uint32_t total_routes{0};
    bool truncated{false}; // exactly total_routes > routes.size()
    std::vector<TopicRouteInfo> routes;
};
// Capture only routes, including notices; no Topic payload, proxies or application data.
// Gateway identity/session must be valid. ACTIVE/CLOSED routes require both valid
// endpoints and nonzero sessions/route_id. WAITING/REJECTED may instead be source-only
// notices with an entirely zero destination, sessions and route_id, and sending=false.
// All input routes are validated, including ones omitted by the size cap.
int MakeNetworkDiagnosticsSnapshot(const HostIdentity& host, const HostIdentity& boot,
    std::uint64_t session, const TopicDiagnostics&, NetworkDiagnosticsSnapshot&);
// Success may encode only a prefix; total_routes is preserved and truncated is set.
// Outputs are unchanged on error. Errors: -EINVAL (invalid input), -EMSGSIZE
// (size/count), -EPROTONOSUPPORT (version), -EPROTO (malformed wire).
// Wire v1, big endian: magic "KND1" u32, version u16, flags u16 (bit0 truncated),
// size u32, host[16], boot[16], session u64, total u32, included u32 (60 bytes).
// Each route: source/destination (host[16],boot[16],pid u32,ticks u64,registration u64),
// source/destination sessions u64, route_id u64, state/sending/reason/proxy_recovery u8,
// error signed two's-complement i32, nine u64 counters in TopicRouteInfo order,
// name length u16, name bytes (absolute, 1..240 bytes, no controls). No padding.
int EncodeNetworkDiagnosticsSnapshot(const NetworkDiagnosticsSnapshot&, std::vector<std::uint8_t>&);
int DecodeNetworkDiagnosticsSnapshot(const std::uint8_t*, std::size_t, NetworkDiagnosticsSnapshot&);
}
