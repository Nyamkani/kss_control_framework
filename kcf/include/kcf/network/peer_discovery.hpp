#pragma once
#include "kcf/network/discovery_protocol.hpp"
#include <chrono>
#include <memory>

namespace kcf::network
{
enum class PeerState : std::uint8_t { ONLINE = 1, LOST = 2 };
struct PeerInfo
{
    HostIdentity host, boot_id;
    std::uint64_t session_id{0};
    std::string address; // observed IPv4 source, not an identity
    std::uint16_t port{0}; // unicast metadata socket
    std::chrono::steady_clock::time_point last_seen;
    std::uint64_t metadata_revision{0};
    PeerState state{PeerState::ONLINE};
    PeerProfile profile;
    CompatibilityResult compatibility; // version-level only, independent of ONLINE/LOST
    bool metadata_ready{false};
    std::vector<NetworkEndpointMetadata> endpoints;
};
struct DiscoveryConfig
{
    HostIdentity host, boot_id;
    PeerProfile profile;
    CompatibilityPolicy compatibility_policy{CompatibilityPolicy::NETWORK_PROTOCOL};
    std::string interface_address{"0.0.0.0"};
    std::string multicast_group{"239.255.75.67"};
    std::uint16_t port{37651};
    std::uint16_t metadata_port{0}; // optional UDP bind port; Control uses same numeric TCP port
    std::chrono::milliseconds announce_interval{1000};
    std::chrono::milliseconds peer_timeout{4000};
    std::chrono::milliseconds metadata_retry{200};
    std::size_t max_peers{DISCOVERY_MAX_PEERS};
};
// Evaluates a snapshot only: callers must re-check fresh state before dispatch.
CompatibilityResult CheckRouteCompatibility(
    const DiscoveryConfig& local, const PeerInfo& peer,
    const NetworkEndpointMetadata& local_endpoint, const RemoteEndpointIdentity& remote_endpoint,
    TargetScope target = TargetScope::ENDPOINT);
struct DiscoveryStats
{
    std::uint64_t announces_sent{0}, metadata_requests_sent{0}, metadata_responses_sent{0};
    std::uint64_t rejected_datagrams{0};
};
// One instance per Host ID. Start/Stop/destruction are caller-serialized.
// UpdateMetadata/GetPeers/GetStats are safe concurrently while started. No
// callbacks or Local IPC hooks. Snapshot APIs allocate and are not hard real-time.
class PeerDiscovery
{
public:
    PeerDiscovery();
    ~PeerDiscovery();
    PeerDiscovery(const PeerDiscovery&) = delete;
    PeerDiscovery& operator=(const PeerDiscovery&) = delete;
    int Start(const DiscoveryConfig&, const std::vector<NetworkEndpointMetadata>& endpoints = {});
    int UpdateMetadata(const std::vector<NetworkEndpointMetadata>&);
    void Stop(); // wakes poll, joins worker, closes all sockets; idempotent
    std::vector<PeerInfo> GetPeers() const;
    DiscoveryStats GetStats() const;
    std::uint64_t GetSessionId() const;
    int GetLastError() const; // discovery-only error, never Runtime error
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace kcf::network
