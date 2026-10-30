#pragma once
#include "kcf/network/peer_discovery.hpp"
#include "kcf/network/local_metadata.hpp"
#include "kcf/network/control_frame.hpp"
#include "kcf/network/topic_data_plane.hpp"
#include <future>
#include <mutex>
namespace kcf::network {
struct ControlConfig {
    TopicConfig topics;
    DiscoveryConfig discovery;
    MetadataConfig metadata; // host/boot are supplied from discovery
    std::chrono::milliseconds metadata_interval{250};
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds reconnect_interval{250};
    // Read-only diagnostics on the existing listener, queried via NetworkDiagnosticsClient.
    // Opt-in: at most 8 observer connections / queued jobs, one job per connection,
    // 10 admissions/s per Host session, 50 dispatches/s globally. Observer output
    // is capped at 32 KiB / 16 frames. Overload yields -EAGAIN or closes a full queue.
    bool diagnostics_enabled{false};
    // Explicit LAN policy, NOT authentication. Default denies dedicated clients.
    // At most 32 observer Host IDs may issue ENDPOINT Parameter Get only.
    // Existing remote_parameters allowlist and type/owner checks still apply.
    std::vector<HostIdentity> parameter_get_clients;
    // Independent explicit Set grant, max 32 Hosts, default deny. No Get grant
    // implied. Same unauthenticated trusted-LAN policy and REMOTE endpoint checks.
    std::vector<HostIdentity> parameter_set_clients;
    // Independent ENDPOINT Service Call grant; max 32 Hosts, default deny.
    // Does not grant Parameter access or Action; not authenticated authorization.
    std::vector<HostIdentity> service_call_clients;
    // Independent ENDPOINT Action grants; each max 32 Hosts, default deny.
    std::vector<HostIdentity> action_goal_clients, action_cancel_clients;
    // Read-only schema queries, independent of all Control/Diagnostics grants.
    // Default empty, max 32 observer Hosts. Only REMOTE
    // registered endpoints are visible. Trusted LAN policy, not authentication.
    std::vector<HostIdentity> descriptor_query_clients;
    // Opt-in trusted-LAN read-only schema access without a Host allowlist.
    // Only current REMOTE Topic PROVIDER VALUE descriptors; no Control grants.
    // Advertises TYPE_DESCRIPTOR_QUERY; existing observer resource caps apply.
    bool topic_observer_enabled{false};
    // Observer (DISCOVERY-only or receive-only Topic profile) connections: at most 8 total (shared with Diagnostics),
    // same Host/Boot/Session must also use the same HELLO Runtime identity.
    // Dedicated Control replay window: 256 IDs, max 192 Hosts, retained until
    // Start; unseen out-of-order IDs inside the window allowed, older IDs denied.
    // Normal gateway-to-gateway connection direction/deduplication is unchanged.
};
struct RemoteActionSnapshot {
    RemoteGoalIdentity goal;
    RemoteEndpointIdentity endpoint;
    bool accepted{false},terminal{false},communication_lost{false};
    bool execution_unknown{false},cancel_unknown{false},cancel_accepted{false};
    int error{0};ActionState state{ActionState::IDLE};
    std::uint64_t feedback_sequence{0};
    std::vector<std::uint8_t> feedback,result; // latest feedback, canonical field bytes
};
class RemoteActionSession {
public:
    RemoteActionSnapshot Snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return snapshot_;}
private:
    friend class ControlNode;
    mutable std::mutex mutex_;RemoteActionSnapshot snapshot_;ControlFrame request_;
};
struct ParameterEndpointResult {
    RemoteEndpointIdentity endpoint;
    int error{0};
    CompatibilityReason reason{CompatibilityReason::COMPATIBLE};
    std::vector<std::uint8_t> value; // canonical field codec bytes
    std::shared_ptr<RemoteActionSession> action;
    bool cancel_unknown{false};
    bool execution_unknown{false}; // Service error after dispatch: may have executed, never auto-retry
};
struct ParameterBatchResult {
    int error{0}; // admission/selection error only; always inspect each endpoint
    std::vector<ParameterEndpointResult> endpoints;
};
using ActionBatchResult = ParameterBatchResult;
using ServiceEndpointResult = ParameterEndpointResult;
using ServiceBatchResult = ParameterBatchResult;
struct ControlStats {
    std::uint64_t connections{0},requests_sent{0},responses_ignored{0},rejected_frames{0};
};
// One host-level gateway, no Application Network registration. Start/Stop are
// caller-serialized; Submit/GetPeers/GetStats are thread-safe. No user callbacks.
class ControlNode {
public:
    ControlNode();~ControlNode();
    ControlNode(const ControlNode&)=delete;
    ControlNode& operator=(const ControlNode&)=delete;
    int Start(const ControlConfig&);
    void Stop();
    // Nonblocking admission. Each endpoint progresses independently; no request
    // is automatically retried (a timed-out Set may already have committed).
    std::future<ParameterBatchResult> Get(const RemoteTarget&,const std::string&,const TypeIdentity&,
        std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
    std::future<ParameterBatchResult> Set(const RemoteTarget&,const std::string&,const TypeIdentity&,
        const std::vector<std::uint8_t>&,std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
    // Timeout/disconnect does NOT mean the remote callback was not executed.
    std::future<ServiceBatchResult> CallService(const RemoteTarget&,const std::string&,
        const TypeIdentity& request_type,const TypeIdentity& response_type,const std::vector<std::uint8_t>& request,
        std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
    std::future<ActionBatchResult> StartAction(const RemoteTarget&,const std::string&,
        const TypeIdentity& goal,const TypeIdentity& feedback,const TypeIdentity& result,
        const std::vector<std::uint8_t>& payload,std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
    // A successful Cancel acknowledges the request, not terminal completion.
    std::future<ActionBatchResult> CancelAction(const std::shared_ptr<RemoteActionSession>&,
        std::chrono::milliseconds timeout=std::chrono::milliseconds(1000));
    std::vector<PeerInfo> GetPeers() const;
    // Read-only local Discovery generation; Start/Stop retain caller serialization.
    std::uint64_t GetSessionId() const;
    ControlStats GetStats() const;
    TopicDiagnostics GetTopicDiagnostics() const;
    int GetLastError() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
