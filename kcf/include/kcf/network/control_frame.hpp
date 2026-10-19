#pragma once
#include "kcf/network/parameter_codec.hpp"
#include "kcf/network/compatibility.hpp"
#include "kcf/action/action_state.hpp"
namespace kcf::network {
inline constexpr std::uint32_t CONTROL_MAGIC=0x4b434643; // KCFC
inline constexpr std::uint16_t CONTROL_WIRE_VERSION=1;
inline constexpr std::size_t CONTROL_PREFIX_SIZE=16;
inline constexpr std::size_t MAX_CONTROL_FRAME=MAX_PARAMETER_WIRE+1024;
enum class ControlMessage : std::uint16_t { HELLO=1, PARAM_GET_REQUEST, PARAM_GET_RESPONSE, PARAM_SET_REQUEST, PARAM_SET_RESPONSE, SERVICE_REQUEST, SERVICE_RESPONSE, ACTION_GOAL_REQUEST, ACTION_GOAL_RESPONSE, ACTION_FEEDBACK, ACTION_RESULT, ACTION_CANCEL_REQUEST, ACTION_CANCEL_RESPONSE, DIAGNOSTICS_REQUEST=14, DIAGNOSTICS_RESPONSE=15 };
struct RemoteGoalIdentity {
    RemoteRuntimeIdentity source;std::uint64_t goal_id{0},session_id{0};
    bool operator==(const RemoteGoalIdentity& other) const {return source==other.source&&goal_id==other.goal_id&&session_id==other.session_id;}
    bool operator!=(const RemoteGoalIdentity& other) const {return !(*this==other);}
};
inline bool IsActionMessage(ControlMessage op){return op>=ControlMessage::ACTION_GOAL_REQUEST&&op<=ControlMessage::ACTION_CANCEL_RESPONSE;}
inline bool IsDiagnosticsMessage(ControlMessage op){return op==ControlMessage::DIAGNOSTICS_REQUEST||op==ControlMessage::DIAGNOSTICS_RESPONSE;}
struct ControlFrame {
    ControlMessage operation{ControlMessage::HELLO};
    RemoteRuntimeIdentity source;
    std::uint64_t session_id{0},request_id{0};
    RemoteEndpointIdentity endpoint; // resolved target, retained on response
    TargetScope target{TargetScope::ENDPOINT};
    std::string name;
    TypeIdentity type; // Service: request type, including in response correlation
    TypeIdentity response_type; // Service only: 24-byte operation-specific extension
    TypeIdentity feedback_type;
    RemoteGoalIdentity goal;
    ActionState action_state{ActionState::IDLE};
    bool outcome_unknown{false}; // error response: Local dispatch may have applied
    std::uint64_t feedback_sequence{0};
    std::uint32_t timeout_ms{0};
    std::int32_t status{0}; // 0 or negative errno
    CompatibilityReason reason{CompatibilityReason::COMPATIBLE};
    std::vector<std::uint8_t> payload;
};
// Diagnostics use the unchanged 172-byte common header, target=HOST, empty
// endpoint/name/type and no Service/Action extensions. Request: nonzero request_id,
// timeout 1..60000 ms, zero status/reason and empty payload. Response: same request_id,
// timeout=0; success carries a valid F-ND1 snapshot (<=16 KiB) whose gateway
// host/boot/session equals the response source/session; failure has negative errno
// and empty payload. Known CompatibilityReason values are allowed on failures.
// Request/response correlation and authenticated Peer/HELLO binding require the
// caller's connection state (F-ND2B); a standalone codec cannot verify them.
int EncodeControlFrame(const ControlFrame&,std::vector<std::uint8_t>&);
int DecodeControlFrame(const std::uint8_t*,std::size_t,ControlFrame&);
// Validate prefix before allocating a body; -EAGAIN means more prefix needed.
int ControlFrameSize(const std::uint8_t*,std::size_t,std::size_t&);
}
