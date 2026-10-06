#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kcf::network
{
// In-process contracts, NOT native-memory wire or SHM layouts. Local IPC types
// remain unchanged. Discovery has its own envelope; payload codecs are future work.
// Endpoint/control-model schema, NOT PeerProfile::network or Discovery version.
inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
inline constexpr std::size_t MAX_CONTROL_PAYLOAD = 1024 * 1024;

struct HostIdentity
{
    std::array<std::uint8_t, 16> bytes{};
};
inline bool operator==(const HostIdentity& a, const HostIdentity& b) { return a.bytes == b.bytes; }
inline bool operator!=(const HostIdentity& a, const HostIdentity& b) { return !(a == b); }

// Provision host persistently; boot_id identifies a boot session, preventing
// PID/start_ticks reuse after reboot. Neither hostname nor IP is identity.
struct RemoteRuntimeIdentity
{
    HostIdentity host;
    HostIdentity boot_id;
    std::int32_t pid{0};
    std::uint64_t process_start_ticks{0};
};
inline bool operator==(const RemoteRuntimeIdentity& a, const RemoteRuntimeIdentity& b)
{
    return a.host == b.host && a.boot_id == b.boot_id && a.pid == b.pid &&
           a.process_start_ticks == b.process_start_ticks;
}
inline bool operator!=(const RemoteRuntimeIdentity& a, const RemoteRuntimeIdentity& b) { return !(a == b); }
struct RemoteEndpointIdentity
{
    RemoteRuntimeIdentity runtime;
    std::uint64_t registration_id{0};
};
inline bool operator==(const RemoteEndpointIdentity& a, const RemoteEndpointIdentity& b)
{
    return a.runtime == b.runtime && a.registration_id == b.registration_id;
}
inline bool operator!=(const RemoteEndpointIdentity& a, const RemoteEndpointIdentity& b) { return !(a == b); }

enum class TargetScope : std::uint8_t { ENDPOINT = 1, HOST, APPLICATION, GROUP, ALL };
struct RemoteTarget
{
    TargetScope scope{TargetScope::ENDPOINT};
    RemoteEndpointIdentity endpoint;
    HostIdentity host;
    RemoteRuntimeIdentity application; // Supervisor instance, not display name
    std::string group;
    // Exactly one selector is populated. ALL has no selectors. No broadcast
    // addresses or special service names such as /stop_all are introduced.
};

enum class TrafficClass : std::uint8_t { CONTROL = 1, TOPIC };
enum class Priority : std::uint8_t { NORMAL = 1, HIGH };
enum class NetworkScope : std::uint8_t { LOCAL = 0, REMOTE = 1 };
// AUTO is intentionally not accepted until its policy is defined.
enum class EndpointKind : std::uint8_t { TOPIC = 1, PARAMETER, SERVICE, ACTION };
enum class EndpointRole : std::uint8_t { PROVIDER = 1, CONSUMER };
enum class ControlOperation : std::uint16_t
{
    PARAM_GET = 1, PARAM_SET = 2,
    SERVICE_REQUEST = 3, SERVICE_RESPONSE = 4,
    ACTION_GOAL = 5, ACTION_FEEDBACK = 6, ACTION_RESULT = 7, ACTION_CANCEL = 8
};
// Explicit encoding/schema identifiers must be negotiated before dispatch.
// Native Local Payload bytes are NOT automatically a portable network format.
struct TypeIdentity
{
    std::uint64_t type_id{0};
    std::uint64_t schema_id{0}; // wire schema, not Local SHM layout_id
    std::uint32_t encoding_id{0}; // configured codec identifier; 0 is invalid
    std::uint32_t payload_size{0}; // this base supports fixed-size payloads
};
inline bool operator==(const TypeIdentity& a, const TypeIdentity& b)
{
    return a.type_id == b.type_id && a.schema_id == b.schema_id &&
           a.encoding_id == b.encoding_id && a.payload_size == b.payload_size;
}

struct NetworkEndpointMetadata
{
    std::uint16_t protocol_version{PROTOCOL_VERSION};
    RemoteEndpointIdentity identity;
    RemoteRuntimeIdentity application; // all-zero for Standalone
    std::string host_name; // diagnostic only
    std::string application_name;
    std::string element_name;
    std::string name;
    std::vector<std::string> groups; // trusted configuration, not authorization
    EndpointKind kind{EndpointKind::TOPIC};
    EndpointRole role{EndpointRole::PROVIDER};
    NetworkScope scope{NetworkScope::LOCAL};
    TypeIdentity value_type; // Topic/Parameter
    TypeIdentity request_type, response_type; // Service request/response or Action goal/result
    TypeIdentity feedback_type; // Action only
};

struct ActionGoalIdentity
{
    RemoteRuntimeIdentity source;
    std::uint64_t request_id{0};
};
struct RemoteRequest
{
    std::uint16_t protocol_version{PROTOCOL_VERSION};
    // Unique within source runtime session; retries keep the same request_id.
    // Request identity is (source, request_id), not request_id alone.
    RemoteRuntimeIdentity source;
    std::uint64_t request_id{0};
    ControlOperation operation{ControlOperation::PARAM_GET};
    RemoteTarget target;
    ActionGoalIdentity goal; // required only for ACTION_RESULT / ACTION_CANCEL
    std::string endpoint_name;
    std::uint32_t timeout_ms{0}; // finite requester-local duration, not remote clock time
    TypeIdentity payload_type, response_type;
    std::vector<std::uint8_t> payload;
};
enum class RemoteStatus : std::uint8_t
{
    PENDING = 0, SUCCESS, TIMEOUT, DISCONNECTED, LOST, REJECTED, REMOTE_ERROR
};
struct RemoteEndpointResult
{
    RemoteRuntimeIdentity source; // echoes requester, not responder
    std::uint64_t request_id{0};
    RemoteEndpointIdentity endpoint;
    RemoteStatus status{RemoteStatus::PENDING};
    std::int32_t error{0}; // nonzero only for REJECTED/REMOTE_ERROR
    TypeIdentity payload_type;
    std::vector<std::uint8_t> payload;
};
// Selected endpoints are frozen before parallel dispatch. Each has its own
// PENDING/final result, deadline and error; no aggregate success hides failures.
struct RemoteRequestResult
{
    std::vector<RemoteEndpointResult> endpoints;
};
// Action goal uses (source, request_id) as its initial goal correlation key.
// Later requests have independent request IDs and explicitly refer to that key.
struct RemoteActionEvent
{
    ActionGoalIdentity goal;
    RemoteEndpointIdentity endpoint;
    ControlOperation operation{ControlOperation::ACTION_FEEDBACK};
    TypeIdentity payload_type;
    std::vector<std::uint8_t> payload;
};
} // namespace kcf::network
