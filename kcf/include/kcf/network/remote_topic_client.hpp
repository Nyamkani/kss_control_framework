#pragma once
#include "kcf/network/remote_control_client.hpp"
#include "kcf/network/topic_data_plane.hpp"
#include <mutex>
namespace kcf::network {
enum class RemoteTopicState { WAITING_ROUTE, ACTIVE, LOST, STALE, ERROR, CLOSED };
struct RemoteTopicSnapshot {
    RemoteTopicState state{RemoteTopicState::WAITING_ROUTE};int error{0};bool has_value{false};
    TypeDescriptor descriptor; // packed buffer layout, never remote native ABI
    std::vector<std::uint8_t> payload; // canonical big-endian, latest only
    std::uint64_t sequence{0},sample_sequence{0};
    // missed_sequence: transport gaps (including possible sender send errors),
    // NOT source sampling skips. Query sender Diagnostics.local_samples_skipped
    // for sampling skips; never label sample_sequence gaps as packet loss.
    TopicRouteInfo route;std::string proxy_name;
};
class RemoteTopicSubscription {
public: RemoteTopicSnapshot Snapshot() const;
private:
    friend class TopicDataPlane;friend class RemoteTopicClient;
    struct State;std::shared_ptr<State> state_;
    explicit RemoteTopicSubscription(std::shared_ptr<State> state):state_(std::move(state)){}
};
struct RemoteTopicConfig {
    std::uint16_t publisher_port{37652},receive_port{0}; // 0 => OS-assigned, no SO_REUSEPORT
    std::size_t max_subscriptions{32}; // 1..64; each latest canonical buffer <=768 bytes
    std::uint32_t socket_buffer_bytes{64*1024};
};
// Shares a running Discovery with Control/Diagnostics; initialize its profile to
// DISCOVERY | REMOTE_TOPIC_RECEIVER | TARGET_ENDPOINT before Start. Discovery and
// descriptor client must outlive this object. Serialize Start/Stop; API calls are
// worker-thread calls. Join callers before destruction. No listener or second
// Discovery. One bounded TopicDataPlane worker/socket. No retry or auto reselect.
// A separate Gateway on the same PC uses a distinct logical HostIdentity (the
// existing one-Discovery-per-Host contract), even when sharing the same IPv4.
// Start the shared discovery before this client; Stop this client before Discovery.
// Additive Consumer metadata survives base UpdateMetadata calls. Subscription IDs
// are process-wide monotonic; never fork a running client. Terminal/LOST handles
// retain historical data and occupy a slot until Unsubscribe or Stop.
// Publisher needs TOPIC_ENDPOINT_SUBSCRIPTION and descriptor-query permission
// (explicit Host allowlist or Gateway topic_observer_enabled opt-in).
// GetDiagnostics is the receive-side snapshot; NetworkDiagnosticsClient queries
// the sender Gateway. This client does not create a Diagnostics TCP listener.
// Trusted LAN only: peer/session checks are not cryptographic authentication.
class RemoteTopicClient {
public:
    RemoteTopicClient(PeerDiscovery&,const DiscoveryConfig&,RemoteControlClient&);
    ~RemoteTopicClient();
    int Start(const RemoteTopicConfig& = {});
    int Subscribe(const RemoteEndpointIdentity&,std::uint64_t expected_session,
        const std::string&,const TypeIdentity&,std::shared_ptr<RemoteTopicSubscription>&,
        std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
    int Unsubscribe(const std::shared_ptr<RemoteTopicSubscription>&);
    void Stop(); // closes handles, joins worker, releases only owned proxy names
    TopicDiagnostics GetDiagnostics() const;
    std::uint16_t ReceivePort() const;
private:
    PeerDiscovery& discovery_;DiscoveryConfig discovery_config_;RemoteControlClient& control_;
    mutable std::mutex mutex_;TopicDataPlane plane_;bool running_{false};std::uint64_t generation_{0};
};
}
