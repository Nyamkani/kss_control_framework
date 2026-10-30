#pragma once
#include "kcf/network/peer_discovery.hpp"
#include <atomic>
#include <mutex>
#include <memory>
#include "kcf/network/control_node.hpp"

namespace kcf::network {
struct RemoteParameterSetResult {
    int error{0};
    // Conservative: request transmission attempted, but application not confirmed.
    // Never retry automatically, even after timeout/Stop/disconnect.
    bool execution_unknown{false};
    bool response_received{false}; // correlated response from the selected generation
};
struct RemoteServiceCallResult {
    int error{0};
    bool execution_unknown{false};
    bool response_received{false};
    std::vector<std::uint8_t> value; // canonical response, empty on error
};
// Handle owns only observable session state; Snapshot remains valid after Stop.
class RemoteControlActionHandle {
public:
    RemoteActionSnapshot Snapshot() const;
private:
    friend class RemoteControlClient;
    struct State;
    explicit RemoteControlActionHandle(std::shared_ptr<State> state):state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
struct RemoteActionStartResult {
    int error{0};bool response_received{false},execution_unknown{false};
    std::shared_ptr<RemoteControlActionHandle> handle;
};
struct RemoteActionCancelResult {
    int error{0};bool response_received{false},cancel_unknown{false},accepted{false};
};
// Blocking worker-thread API; not a GUI callback API. Borrows an already running
// observer PeerDiscovery (DISCOVERY only, or receive-only Topic profile)
// and its exact Start config. No discovery, listener
// or Topic gateway is created. Discovery must outlive the client and remain
// running until Stop returns and all callers have joined. Serialize Start/Stop;
// Stop may run concurrently with all request APIs. Join callers before destruction.
// One synchronous Get/Set/Service request per client, independent of up to four
// Action workers/connections. No retry. Actions retain their own TCP connection.
// Request IDs are process-wide monotonic; do not fork a running discovery/client.
// Limited connections share the Gateway observer cap (8); Get uses the existing
// bounded Control job queue (64), with separate replay records (192 allowlisted Hosts; 256-ID replay window).
// Diagnostics may run concurrently on its own connection. Gateway permits eight
// limited connections total; exhaustion rejects connections. Same discovery
// incarnation must use the same HELLO Runtime identity.
// Gateway must explicitly allow our Host in parameter_get_clients for Get,
// parameter_set_clients for Set, or service_call_clients for Service. Grants are independent.
// This is an unauthenticated trusted-LAN policy, not cryptographic authorization:
// spoofing Host/IP/HELLO is not prevented. Service requires its separate allowlist; Action Goal/Cancel require separate explicit grants.
class RemoteControlClient {
public:
    RemoteControlClient(PeerDiscovery&, const DiscoveryConfig&);
    ~RemoteControlClient();
    RemoteControlClient(const RemoteControlClient&)=delete;
    RemoteControlClient& operator=(const RemoteControlClient&)=delete;
    int Start();
    void Stop(); // cancels IO/queued requests; waits for active socket cleanup
    // Endpoint and gateway_session must come from the SAME selected PeerInfo.
    // Exact endpoint/name/type only; never reselects a newer generation. Success
    // returns canonical field bytes (DecodeParameterValue with known descriptor).
    // Output unchanged on error. timeout 1..60000 ms covers queue/connect/HELLO/IO.
    // -ESTALE: boot/session changed; -ENOTCONN: LOST/not started;
    // -ENOENT: unknown peer/endpoint; -EAGAIN: metadata not ready;
    // -EPROTONOSUPPORT: incompatible peer; -EPROTOTYPE: wrong type;
    // -EACCES: LOCAL-only; -ECANCELED: Stop; -ETIMEDOUT: deadline;
    // other negative socket/codec/Gateway errors propagate. A legacy/denying
    // gateway may close HELLO (-ECONNRESET); no explicit grant negotiation exists.
    int GetParameter(const RemoteEndpointIdentity&, std::uint64_t gateway_session,
        const std::string& name, const TypeIdentity&, std::vector<std::uint8_t>& value,
        std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
    // Same identity/deadline contract as Get. Payload uses EncodeParameterValue.
    // Gateway parameter_set_clients is independent of parameter_get_clients.
    // error==0 only after a matching successful remote Set response.
    // Preflight/HELLO failures and explicit -EACCES are known-not-applied.
    // After send is attempted, other failures conservatively remain unknown,
    // including negative replies: existing wire does not identify commit stage.
    RemoteParameterSetResult SetParameter(const RemoteEndpointIdentity&, std::uint64_t gateway_session,
        const std::string& name, const TypeIdentity&, const std::vector<std::uint8_t>& payload,
        std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
    // Exact ENDPOINT only. Full request/response TypeIdentity checks. Worker call,
    // same serialization/Stop contract as Get/Set. No retry. Negative Service
    // replies conservatively leave execution_unknown=true except explicit EACCES;
    // the existing wire cannot distinguish a callback error from pre-dispatch error.
    RemoteServiceCallResult CallService(const RemoteEndpointIdentity&, std::uint64_t expected_session,
        const std::string& name, const TypeIdentity& request_type, const TypeIdentity& response_type,
        const std::vector<std::uint8_t>& request,
        std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
    // Read-only schema lookup, exact endpoint/session/name/advertised TypeIdentity.
    // VALUE: Parameter/Topic; REQUEST/RESPONSE: Service or Action goal/result;
    // FEEDBACK: Action only. Unknown names/layouts are learned from the result.
    // Returns a packed buffer descriptor, never the remote native struct layout.
    // DecodeParameterValue/EncodeParameterValue operate on that buffer; use memcpy
    // for native scalar access, not typed casts. Output unchanged on failure.
    // Uses the existing bounded/deadline/Stop path, independent of Action workers.
    // Requires descriptor_query_clients or topic_observer_enabled (REMOTE Topic
    // PROVIDER VALUE only); absent peer capability => -ENOTSUP
    // before connecting. No retries. Existing protocol/storage versions unchanged.
    int QueryTypeDescriptor(const RemoteEndpointIdentity&, std::uint64_t expected_session,
        const std::string& name, DescriptorRole, const TypeIdentity& expected_type,
        TypeDescriptor&, std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
    // ACK timeout 1..60000 ms; tracking timeout 1..3600000 ms after acceptance.
    // Latest-only Feedback, no automatic reconnect. At most four live Actions per
    // client; admission fails with EAGAIN. Caller may retain the returned handle.
    RemoteActionStartResult StartAction(const RemoteEndpointIdentity&, std::uint64_t expected_session,
        const std::string&, const TypeIdentity& goal, const TypeIdentity& feedback, const TypeIdentity& result,
        const std::vector<std::uint8_t>& payload,
        std::chrono::milliseconds ack_timeout=std::chrono::milliseconds{1000},
        std::chrono::milliseconds tracking_timeout=std::chrono::milliseconds{60000});
    // Uses only the immutable Goal Identity/connection of this client's handle.
    // One pending Cancel per handle; ACK is NOT terminal CANCELED. Stop never sends
    // Cancel. A terminal/detached/foreign handle cannot be reused on a new session.
    RemoteActionCancelResult CancelAction(const std::shared_ptr<RemoteControlActionHandle>&,
        std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
private:
    void RunAction(const std::shared_ptr<RemoteControlActionHandle::State>&);
    std::mutex actions_mutex_;
    std::vector<std::shared_ptr<RemoteControlActionHandle::State>> actions_;
    int ControlRequest(const RemoteEndpointIdentity&, std::uint64_t,
        const std::string&, const TypeIdentity&, std::vector<std::uint8_t>&,
        std::chrono::milliseconds, const std::vector<std::uint8_t>*, RemoteParameterSetResult*, const TypeIdentity* response_type=nullptr, DescriptorRole role=DescriptorRole::NONE);
    PeerDiscovery& discovery_;
    DiscoveryConfig config_;
    std::timed_mutex mutex_;
    std::atomic<bool> running_{false},canceled_{true};
    std::atomic<std::uint64_t> generation_{0};
};
}
