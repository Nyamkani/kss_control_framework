#pragma once
#include "kcf/network/topic_data_plane.hpp"
#include <cerrno>
#include <cstdint>

namespace kcf::network {
// Evidence-backed diagnostic labels. A single route's current state cannot
// establish root cause for a missing Publisher, OS multicast loss, or Wi-Fi.
enum class DiagnosticStage : std::uint8_t {
    UNKNOWN, METADATA, EXPOSURE, COMPATIBILITY, AUTHORIZATION, ROUTE, TRANSPORT
};
enum class DiagnosticCode : std::uint8_t {
    UNKNOWN, OK, WAITING_SUBSCRIBER, LOCAL_ONLY, TYPE_MISMATCH,
    METADATA_UNAVAILABLE, PEER_LOST, CAPABILITY_MISSING,
    PERMISSION_DENIED, ROUTE_REJECTED, ROUTE_CLOSED, TRANSPORT_ERROR
};
struct DiagnosticResult {
    DiagnosticStage stage{DiagnosticStage::UNKNOWN};
    DiagnosticCode code{DiagnosticCode::UNKNOWN};
    int error{0};
    CompatibilityReason reason{CompatibilityReason::COMPATIBLE};
};
inline const char* DiagnosticCodeName(DiagnosticCode code) {
    switch(code) {
    case DiagnosticCode::OK:return "OK";
    case DiagnosticCode::WAITING_SUBSCRIBER:return "WAITING_SUBSCRIBER";
    case DiagnosticCode::LOCAL_ONLY:return "LOCAL_ONLY";
    case DiagnosticCode::TYPE_MISMATCH:return "TYPE_MISMATCH";
    case DiagnosticCode::METADATA_UNAVAILABLE:return "METADATA_UNAVAILABLE";
    case DiagnosticCode::PEER_LOST:return "PEER_LOST";
    case DiagnosticCode::CAPABILITY_MISSING:return "CAPABILITY_MISSING";
    case DiagnosticCode::PERMISSION_DENIED:return "PERMISSION_DENIED";
    case DiagnosticCode::ROUTE_REJECTED:return "ROUTE_REJECTED";
    case DiagnosticCode::ROUTE_CLOSED:return "ROUTE_CLOSED";
    case DiagnosticCode::TRANSPORT_ERROR:return "TRANSPORT_ERROR";
    default:return "UNKNOWN";
    }
}
inline DiagnosticResult DiagnoseTopicRoute(const TopicRouteInfo& route) {
    DiagnosticResult r;
    r.error=route.error;
    r.reason=route.reason;
    if(route.state==TopicRouteState::ACTIVE && route.error==0)
        return {DiagnosticStage::ROUTE,DiagnosticCode::OK,0,route.reason};
    if(route.state==TopicRouteState::WAITING_SUBSCRIBER && route.error==0)
        return {DiagnosticStage::ROUTE,DiagnosticCode::WAITING_SUBSCRIBER,0,route.reason};
    switch(route.reason) {
    case CompatibilityReason::LOCAL_ONLY:
        r.stage=DiagnosticStage::EXPOSURE;r.code=DiagnosticCode::LOCAL_ONLY;return r;
    case CompatibilityReason::TYPE_MISMATCH:
        r.stage=DiagnosticStage::COMPATIBILITY;r.code=DiagnosticCode::TYPE_MISMATCH;return r;
    case CompatibilityReason::METADATA_UNAVAILABLE:
        r.stage=DiagnosticStage::METADATA;r.code=DiagnosticCode::METADATA_UNAVAILABLE;return r;
    case CompatibilityReason::PEER_LOST:
        r.stage=DiagnosticStage::METADATA;r.code=DiagnosticCode::PEER_LOST;return r;
    case CompatibilityReason::CAPABILITY_MISSING:
        r.stage=DiagnosticStage::COMPATIBILITY;r.code=DiagnosticCode::CAPABILITY_MISSING;return r;
    default:break;
    }
    if(route.state==TopicRouteState::CLOSED) {
        r.stage=DiagnosticStage::ROUTE;r.code=DiagnosticCode::ROUTE_CLOSED;return r;
    }
    if(route.error==-EACCES||route.error==-EPERM) {
        r.stage=DiagnosticStage::AUTHORIZATION;r.code=DiagnosticCode::PERMISSION_DENIED;return r;
    }
    if(route.state==TopicRouteState::REJECTED) {
        r.stage=DiagnosticStage::ROUTE;r.code=DiagnosticCode::ROUTE_REJECTED;return r;
    }
    // A transport errno does not prove why transport failed.
    if(route.error && route.state==TopicRouteState::ACTIVE) {
        r.stage=DiagnosticStage::TRANSPORT;r.code=DiagnosticCode::TRANSPORT_ERROR;return r;
    }
    return r;
}
} // namespace kcf::network
