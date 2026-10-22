#pragma once
#include "kcf/network/types.hpp"
#include <algorithm>

namespace kcf::network
{
inline bool Valid(const HostIdentity& id)
{
    return std::any_of(id.bytes.begin(), id.bytes.end(), [](auto b) { return b != 0; });
}
inline bool Valid(const RemoteRuntimeIdentity& id)
{
    return Valid(id.host) && Valid(id.boot_id) && id.pid > 0 && id.process_start_ticks != 0;
}
inline bool Valid(const RemoteEndpointIdentity& id) { return Valid(id.runtime) && id.registration_id != 0; }
inline bool Text(const std::string& value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool Valid(const TypeIdentity& type)
{
    return type.type_id && type.schema_id && type.encoding_id &&
           type.payload_size && type.payload_size <= MAX_CONTROL_PAYLOAD;
}
inline bool Valid(const RemoteTarget& target)
{
    const bool no_endpoint = target.endpoint == RemoteEndpointIdentity{};
    const bool no_host = target.host == HostIdentity{};
    const bool no_application = target.application == RemoteRuntimeIdentity{};
    switch (target.scope) {
    case TargetScope::ENDPOINT: return Valid(target.endpoint) && no_host && no_application && target.group.empty();
    case TargetScope::HOST: return no_endpoint && Valid(target.host) && no_application && target.group.empty();
    case TargetScope::APPLICATION: return no_endpoint && no_host && Valid(target.application) && target.group.empty();
    case TargetScope::GROUP: return no_endpoint && no_host && no_application && Text(target.group, 128);
    case TargetScope::ALL: return no_endpoint && no_host && no_application && target.group.empty();
    }
    return false;
}
inline bool Valid(ControlOperation operation)
{
    switch (operation) {
    case ControlOperation::PARAM_GET: case ControlOperation::PARAM_SET:
    case ControlOperation::SERVICE_REQUEST: case ControlOperation::SERVICE_RESPONSE:
    case ControlOperation::ACTION_GOAL: case ControlOperation::ACTION_FEEDBACK:
    case ControlOperation::ACTION_RESULT: case ControlOperation::ACTION_CANCEL: return true;
    }
    return false;
}
inline bool Classify(TrafficClass traffic, Priority& priority)
{
    switch (traffic) {
    case TrafficClass::CONTROL: priority = Priority::HIGH; return true;
    case TrafficClass::TOPIC: priority = Priority::NORMAL; return true;
    }
    return false;
}
inline bool Classify(ControlOperation operation, Priority& priority)
{
    if (!Valid(operation)) return false;
    return Classify(TrafficClass::CONTROL, priority);
}
inline bool Valid(const NetworkEndpointMetadata& m)
{
    if (m.protocol_version != PROTOCOL_VERSION || !Valid(m.identity) || !Text(m.name, 240) || m.name[0] != '/' ||
        !Text(m.element_name, 128) || (!m.host_name.empty() && !Text(m.host_name, 128)) ||
        (m.role != EndpointRole::PROVIDER && m.role != EndpointRole::CONSUMER) ||
        (m.scope != NetworkScope::LOCAL && m.scope != NetworkScope::REMOTE) || m.groups.size() > 64) return false;
    if (m.application == RemoteRuntimeIdentity{}) {
        if (!m.application_name.empty()) return false;
    } else if (!Valid(m.application) || !Text(m.application_name, 128) ||
               m.application.host != m.identity.runtime.host || m.application.boot_id != m.identity.runtime.boot_id) return false;
    for (std::size_t i = 0; i < m.groups.size(); ++i) {
        if (!Text(m.groups[i], 128) || std::find(m.groups.begin(), m.groups.begin()+i, m.groups[i]) != m.groups.begin()+i) return false;
    }
    const TypeIdentity empty{};
    switch (m.kind) {
    case EndpointKind::TOPIC: case EndpointKind::PARAMETER:
        return Valid(m.value_type) && m.request_type == empty && m.response_type == empty && m.feedback_type == empty;
    case EndpointKind::SERVICE:
        return m.value_type == empty && Valid(m.request_type) && Valid(m.response_type) && m.feedback_type == empty;
    case EndpointKind::ACTION:
        return m.value_type == empty && Valid(m.request_type) && Valid(m.response_type) && Valid(m.feedback_type);
    }
    return false;
}
// A metadata predicate only: caller must have a live, explicitly subscribed
// remote consumer. Does not create a socket/route or infer liveness from metadata.
// Future dispatch must also pass CheckRouteCompatibility; this predicate alone
// does not check peer versions/capabilities.
inline bool CanRouteTopic(const NetworkEndpointMetadata& publisher, const NetworkEndpointMetadata& subscriber,
                          bool active_remote_subscription)
{
    return active_remote_subscription && Valid(publisher) && Valid(subscriber) &&
        publisher.kind == EndpointKind::TOPIC && subscriber.kind == EndpointKind::TOPIC &&
        publisher.role == EndpointRole::PROVIDER && subscriber.role == EndpointRole::CONSUMER &&
        publisher.scope == NetworkScope::REMOTE && subscriber.scope == NetworkScope::REMOTE &&
        publisher.identity.runtime.host != subscriber.identity.runtime.host && publisher.name == subscriber.name &&
        publisher.value_type == subscriber.value_type;
}
inline bool MatchesTarget(const RemoteTarget& target, const NetworkEndpointMetadata& endpoint)
{
    if (!Valid(target) || !Valid(endpoint) || endpoint.scope != NetworkScope::REMOTE ||
        endpoint.role != EndpointRole::PROVIDER || endpoint.kind == EndpointKind::TOPIC) return false;
    switch (target.scope) {
    case TargetScope::ENDPOINT: return target.endpoint == endpoint.identity;
    case TargetScope::HOST: return target.host == endpoint.identity.runtime.host;
    case TargetScope::APPLICATION: return target.application == endpoint.application;
    case TargetScope::GROUP: return std::find(endpoint.groups.begin(), endpoint.groups.end(), target.group) != endpoint.groups.end();
    case TargetScope::ALL: return true;
    }
    return false;
}
inline bool Payload(const TypeIdentity& type, const std::vector<std::uint8_t>& bytes)
{
    return Valid(type) && bytes.size() == type.payload_size;
}
inline bool Valid(const ActionGoalIdentity& goal) { return Valid(goal.source) && goal.request_id; }
inline bool Valid(const RemoteRequest& request)
{
    if (request.protocol_version != PROTOCOL_VERSION || !Valid(request.source) || !request.request_id ||
        !Valid(request.target) || !Text(request.endpoint_name, 240) || request.endpoint_name[0] != '/' || !request.timeout_ms) return false;
    const bool goal_operation = request.operation == ControlOperation::ACTION_RESULT || request.operation == ControlOperation::ACTION_CANCEL;
    if (goal_operation ? !Valid(request.goal) :
        (request.goal.source != RemoteRuntimeIdentity{} || request.goal.request_id != 0)) return false;
    const bool no_payload = request.payload.empty() && request.payload_type == TypeIdentity{};
    switch (request.operation) {
    case ControlOperation::PARAM_GET: case ControlOperation::ACTION_RESULT:
        return no_payload && Valid(request.response_type);
    case ControlOperation::PARAM_SET:
        return Payload(request.payload_type, request.payload) && request.response_type == TypeIdentity{};
    case ControlOperation::SERVICE_REQUEST: case ControlOperation::ACTION_GOAL:
        return Payload(request.payload_type, request.payload) && Valid(request.response_type);
    case ControlOperation::ACTION_CANCEL:
        return no_payload && request.response_type == TypeIdentity{};
    default: return false; // responses and feedback cannot be requests
    }
}
// Selection compatibility, not a live registry, authorization or fan-out engine.
inline bool MatchesRequest(const RemoteRequest& request, const NetworkEndpointMetadata& endpoint)
{
    if (!Valid(request) || !MatchesTarget(request.target, endpoint) || request.endpoint_name != endpoint.name) return false;
    switch (request.operation) {
    case ControlOperation::PARAM_GET: return endpoint.kind == EndpointKind::PARAMETER && request.response_type == endpoint.value_type;
    case ControlOperation::PARAM_SET: return endpoint.kind == EndpointKind::PARAMETER && request.payload_type == endpoint.value_type;
    case ControlOperation::SERVICE_REQUEST: return endpoint.kind == EndpointKind::SERVICE && request.payload_type == endpoint.request_type && request.response_type == endpoint.response_type;
    case ControlOperation::ACTION_GOAL: return endpoint.kind == EndpointKind::ACTION && request.payload_type == endpoint.request_type && request.response_type == endpoint.response_type;
    case ControlOperation::ACTION_RESULT: return endpoint.kind == EndpointKind::ACTION && request.response_type == endpoint.response_type;
    case ControlOperation::ACTION_CANCEL: return endpoint.kind == EndpointKind::ACTION;
    default: return false;
    }
}
inline bool Valid(const RemoteEndpointResult& result, const RemoteRequest& request)
{
    if (!Valid(request) || !Valid(result.endpoint) || result.source != request.source || result.request_id != request.request_id) return false;
    if (result.status == RemoteStatus::SUCCESS) {
        return !result.error && result.payload_type == request.response_type &&
            (request.response_type == TypeIdentity{} ? result.payload.empty() : Payload(result.payload_type, result.payload));
    }
    if (!(result.payload_type == TypeIdentity{}) || !result.payload.empty()) return false;
    switch (result.status) {
    case RemoteStatus::PENDING: case RemoteStatus::TIMEOUT: case RemoteStatus::DISCONNECTED: case RemoteStatus::LOST: return !result.error;
    case RemoteStatus::REJECTED: case RemoteStatus::REMOTE_ERROR: return result.error != 0;
    default: return false;
    }
}
// Frozen selection is supplied explicitly so a missing/foreign endpoint result
// cannot be silently treated as successful. No-match is an error, not success.
inline bool Valid(const RemoteRequestResult& results, const RemoteRequest& request,
                  const std::vector<NetworkEndpointMetadata>& selected)
{
    if (!Valid(request) || selected.empty() || results.endpoints.size() != selected.size()) return false;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (!MatchesRequest(request, selected[i])) return false;
        for (std::size_t j = 0; j < i; ++j) if (selected[i].identity == selected[j].identity) return false;
        std::size_t matches = 0;
        for (const auto& result : results.endpoints) {
            if (result.endpoint == selected[i].identity) {
                if (!Valid(result, request)) return false;
                ++matches;
            }
        }
        if (matches != 1) return false;
    }
    return true;
}
inline bool Valid(const RemoteActionEvent& event, const NetworkEndpointMetadata& endpoint)
{
    if (!Valid(event.goal) || !Valid(endpoint) || endpoint.kind != EndpointKind::ACTION ||
        endpoint.role != EndpointRole::PROVIDER || endpoint.scope != NetworkScope::REMOTE || event.endpoint != endpoint.identity) return false;
    if (event.operation == ControlOperation::ACTION_FEEDBACK) return event.payload_type == endpoint.feedback_type && Payload(event.payload_type, event.payload);
    if (event.operation == ControlOperation::ACTION_RESULT) return event.payload_type == endpoint.response_type && Payload(event.payload_type, event.payload);
    return false;
}
} // namespace kcf::network
