#pragma once

#include "kcf/network/validation.hpp"

namespace kcf::network
{
// These versions describe the peer, independently of the Discovery envelope
// and the existing endpoint/control-model schema (PROTOCOL_VERSION).
struct FrameworkVersion
{
    std::uint16_t major{5}, minor{2}, patch{0};
};
inline bool operator==(const FrameworkVersion& a, const FrameworkVersion& b)
{
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
}
struct NetworkProtocolVersion
{
    std::uint16_t major{1}, minor{0};
};
enum class Capability : std::uint64_t
{
    DISCOVERY = 1ull << 0,
    REMOTE_PARAMETER = 1ull << 1,
    REMOTE_SERVICE = 1ull << 2,
    REMOTE_ACTION = 1ull << 3,
    REMOTE_TOPIC = 1ull << 4,
    TARGET_ENDPOINT = 1ull << 5,
    TARGET_HOST = 1ull << 6,
    TARGET_APPLICATION = 1ull << 7,
    TARGET_GROUP = 1ull << 8,
    TARGET_ALL = 1ull << 9
};
using CapabilitySet = std::uint64_t;
inline constexpr CapabilitySet CapabilityBit(Capability c) { return static_cast<CapabilitySet>(c); }
inline constexpr CapabilitySet KNOWN_CAPABILITIES = (1ull << 10) - 1;
struct PeerProfile
{
    FrameworkVersion framework;
    NetworkProtocolVersion network;
    // Standalone PeerDiscovery defaults to DISCOVERY only. ControlNode sets
    // REMOTE_PARAMETER/SERVICE/ACTION and TARGET_* capabilities on startup;
    // REMOTE_TOPIC is advertised only when the enabled Topic Data Plane starts
    // successfully before discovery. Capability bits are not protocol versions.
    CapabilitySet capabilities{CapabilityBit(Capability::DISCOVERY)};
};
inline bool operator==(const PeerProfile& a, const PeerProfile& b)
{
    return a.framework == b.framework && a.network.major == b.network.major &&
           a.network.minor == b.network.minor && a.capabilities == b.capabilities;
}
inline bool Valid(const PeerProfile& p) { return p.network.major != 0; }
enum class CompatibilityPolicy : std::uint8_t
{
    NETWORK_PROTOCOL = 1, EXACT_FRAMEWORK_VERSION
};
enum class CompatibilityReason : std::uint8_t
{
    COMPATIBLE = 0, INVALID_PROFILE, INVALID_POLICY, NETWORK_MAJOR_MISMATCH,
    FRAMEWORK_VERSION_MISMATCH, CAPABILITY_MISSING, INVALID_ENDPOINT,
    SEMANTIC_MISMATCH, TYPE_MISMATCH, LOCAL_ONLY, HOST_MISMATCH,
    PEER_LOST, METADATA_UNAVAILABLE, UNKNOWN_ENDPOINT
};
struct CompatibilityResult
{
    CompatibilityReason reason{CompatibilityReason::INVALID_PROFILE};
    CapabilitySet common_capabilities{0};
    CapabilitySet missing_capabilities{0};
    bool Compatible() const { return reason == CompatibilityReason::COMPATIBLE; }
};
inline const char* CompatibilityReasonName(CompatibilityReason reason)
{
    switch(reason) {
#define KCF_REASON(name) case CompatibilityReason::name: return #name
    KCF_REASON(COMPATIBLE); KCF_REASON(INVALID_PROFILE); KCF_REASON(INVALID_POLICY);
    KCF_REASON(NETWORK_MAJOR_MISMATCH); KCF_REASON(FRAMEWORK_VERSION_MISMATCH);
    KCF_REASON(CAPABILITY_MISSING); KCF_REASON(INVALID_ENDPOINT); KCF_REASON(SEMANTIC_MISMATCH);
    KCF_REASON(TYPE_MISMATCH); KCF_REASON(LOCAL_ONLY); KCF_REASON(HOST_MISMATCH);
    KCF_REASON(PEER_LOST); KCF_REASON(METADATA_UNAVAILABLE); KCF_REASON(UNKNOWN_ENDPOINT);
#undef KCF_REASON
    }
    return "UNKNOWN";
}
inline CompatibilityResult CheckPeerCompatibility(
    const PeerProfile& local, const PeerProfile& remote,
    CompatibilityPolicy policy = CompatibilityPolicy::NETWORK_PROTOCOL)
{
    CompatibilityResult result;
    if(!Valid(local) || !Valid(remote)) return result;
    if(policy != CompatibilityPolicy::NETWORK_PROTOCOL && policy != CompatibilityPolicy::EXACT_FRAMEWORK_VERSION) {
        result.reason = CompatibilityReason::INVALID_POLICY; return result;
    }
    if(local.network.major != remote.network.major) {
        result.reason = CompatibilityReason::NETWORK_MAJOR_MISMATCH; return result;
    }
    if(policy == CompatibilityPolicy::EXACT_FRAMEWORK_VERSION && !(local.framework == remote.framework)) {
        result.reason = CompatibilityReason::FRAMEWORK_VERSION_MISMATCH; return result;
    }
    result.reason = CompatibilityReason::COMPATIBLE;
    // Unknown bits are retained for diagnostics, never negotiated or used.
    result.common_capabilities = local.capabilities & remote.capabilities & KNOWN_CAPABILITIES;
    return result;
}
inline CompatibilityResult CheckCapability(
    const PeerProfile& local, const PeerProfile& remote, CapabilitySet required,
    CompatibilityPolicy policy = CompatibilityPolicy::NETWORK_PROTOCOL)
{
    auto result = CheckPeerCompatibility(local, remote, policy);
    if(!result.Compatible()) return result;
    result.missing_capabilities = required & ~result.common_capabilities;
    if(result.missing_capabilities) result.reason = CompatibilityReason::CAPABILITY_MISSING;
    return result;
}
inline CapabilitySet TargetCapability(TargetScope scope)
{
    switch(scope) {
    case TargetScope::ENDPOINT: return CapabilityBit(Capability::TARGET_ENDPOINT);
    case TargetScope::HOST: return CapabilityBit(Capability::TARGET_HOST);
    case TargetScope::APPLICATION: return CapabilityBit(Capability::TARGET_APPLICATION);
    case TargetScope::GROUP: return CapabilityBit(Capability::TARGET_GROUP);
    case TargetScope::ALL: return CapabilityBit(Capability::TARGET_ALL);
    }
    return 0;
}
// Pure contract check: symmetric provider/consumer pair. This is not proof of
// peer liveness, metadata freshness, target selection, authorization or a codec
// implementation. Live table users should call CheckRouteCompatibility below
// (declared in peer_discovery.hpp) with a fresh snapshot before dispatch.
inline CompatibilityResult CheckEndpointCompatibility(
    const PeerProfile& local, const PeerProfile& remote,
    const NetworkEndpointMetadata& a, const NetworkEndpointMetadata& b,
    CompatibilityPolicy policy = CompatibilityPolicy::NETWORK_PROTOCOL,
    TargetScope target = TargetScope::ENDPOINT)
{
    auto result = CheckPeerCompatibility(local, remote, policy);
    if(!result.Compatible()) return result;
    const auto fail = [&](CompatibilityReason reason) { result.reason = reason; return result; };
    if(!Valid(a) || !Valid(b)) return fail(CompatibilityReason::INVALID_ENDPOINT);
    if(a.kind != b.kind || a.name != b.name || a.role == b.role || !TargetCapability(target))
        return fail(CompatibilityReason::SEMANTIC_MISMATCH);
    if(a.scope != NetworkScope::REMOTE || b.scope != NetworkScope::REMOTE)
        return fail(CompatibilityReason::LOCAL_ONLY);
    if(a.identity.runtime.host == b.identity.runtime.host) return fail(CompatibilityReason::HOST_MISMATCH);
    Capability operation = Capability::REMOTE_TOPIC;
    switch(a.kind) {
    case EndpointKind::TOPIC: operation = Capability::REMOTE_TOPIC; break;
    case EndpointKind::PARAMETER: operation = Capability::REMOTE_PARAMETER; break;
    case EndpointKind::SERVICE: operation = Capability::REMOTE_SERVICE; break;
    case EndpointKind::ACTION: operation = Capability::REMOTE_ACTION; break;
    }
    result = CheckCapability(local, remote, CapabilityBit(operation) | TargetCapability(target), policy);
    if(!result.Compatible()) return result;
    // Fixed-size wire contracts require the entire type identity to match:
    // type ID alone does not establish schema/encoding/native ABI compatibility.
    if(!(a.value_type == b.value_type) || !(a.request_type == b.request_type) ||
       !(a.response_type == b.response_type) || !(a.feedback_type == b.feedback_type))
        return fail(CompatibilityReason::TYPE_MISMATCH);
    return result;
}
#define KCF_ENDPOINT_CHECK(Name, Kind) \
inline CompatibilityResult Check##Name##Compatibility( \
    const PeerProfile& local, const PeerProfile& remote, \
    const NetworkEndpointMetadata& a, const NetworkEndpointMetadata& b, \
    CompatibilityPolicy policy = CompatibilityPolicy::NETWORK_PROTOCOL, \
    TargetScope target = TargetScope::ENDPOINT) \
{ \
    auto result = CheckEndpointCompatibility(local, remote, a, b, policy, target); \
    if(result.Compatible() && a.kind != EndpointKind::Kind) result.reason = CompatibilityReason::SEMANTIC_MISMATCH; \
    return result; \
}
KCF_ENDPOINT_CHECK(Topic, TOPIC)
KCF_ENDPOINT_CHECK(Parameter, PARAMETER)
KCF_ENDPOINT_CHECK(Service, SERVICE)
KCF_ENDPOINT_CHECK(Action, ACTION)
#undef KCF_ENDPOINT_CHECK
} // namespace kcf::network
