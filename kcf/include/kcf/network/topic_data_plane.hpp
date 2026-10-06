#pragma once
#include "kcf/network/local_metadata.hpp"
#include "kcf/network/peer_discovery.hpp"
#include "kcf/network/topic_protocol.hpp"
#include "kcf/network/detail/proxy_ownership.hpp"
namespace kcf::network {
struct TopicTypeBinding {std::string name;TypeDescriptor descriptor;};
struct TopicRateLimit {std::string name;double max_hz{50};};
struct TopicConfig {
    bool enabled{false};
    std::uint16_t port{37652}; // shared configured unicast port, separate from discovery
    double max_hz{50};
    std::vector<TopicRateLimit> rate_limits;
    std::vector<TopicTypeBinding> local_types; // destination-native bootstrap descriptors
    std::size_t max_routes{64},max_proxies{32};
    std::uint32_t socket_buffer_bytes{64*1024};
};
enum class TopicRouteState : std::uint8_t { WAITING_SUBSCRIBER, ACTIVE, REJECTED, CLOSED };
struct TopicRouteInfo {
    RemoteEndpointIdentity source,destination;std::string name;
    std::uint64_t source_session{0},destination_session{0},route_id{0};
    ProxyRecoveryState proxy_recovery{ProxyRecoveryState::NONE};
    bool sending{false};TopicRouteState state{TopicRouteState::WAITING_SUBSCRIBER};
    int error{0};CompatibilityReason reason{CompatibilityReason::COMPATIBLE};
    std::uint64_t packets_sent{0},packets_received{0},samples_published{0},dropped{0},duplicate{0},out_of_order{0},missed_sequence{0},malformed{0},local_samples_skipped{0};
};
struct TopicProxyInfo {
    ProxyRecoveryState recovery{ProxyRecoveryState::NONE};bool stale_detected{false};
    bool is_proxy{true};RemoteEndpointIdentity identity,source;std::string name;TypeIdentity type;
};
struct TopicDiagnostics {
    std::vector<TopicRouteInfo> routes;
    std::vector<TopicProxyInfo> proxies; // never advertised as forwarding sources
    std::uint64_t malformed{0},stale{0},resource_drops{0},socket_errors{0};
    std::size_t active_send_routes{0},active_receive_routes{0},pending_samples{0};
};
class TopicDataPlane {
public:
    TopicDataPlane();~TopicDataPlane();
    int Start(const TopicConfig&,const DiscoveryConfig&,const MetadataConfig&,const RemoteRuntimeIdentity&,PeerDiscovery&);
    void UpdateLocal(const std::vector<LocalEndpoint>&);
    void Stop(); // retain owned Local proxy mappings for this object's lifetime
    TopicDiagnostics GetDiagnostics() const;
    int GetLastError() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
