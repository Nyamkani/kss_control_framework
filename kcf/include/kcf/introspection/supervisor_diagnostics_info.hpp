#pragma once
#include "kcf/introspection/supervisor_info.hpp"
#include "kcf/system/system_status.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace kcf {
// Read-only, bounded Supervisor detail snapshot. A separate SHM channel keeps
// SupervisorInfo v1 and the established SystemStatus layout unchanged.
inline constexpr std::size_t MAX_SUPERVISOR_DIAGNOSTIC_HISTORY = 64;
struct SupervisorElementDiagnostic {
    char name[128]{};
    std::int32_t pid{-1};
    std::uint8_t running{0},failure_detected{0},exit_info_available{0};
    ElementFailureKind failure_kind{ElementFailureKind::NONE};
    ProcessTerminationKind termination_kind{ProcessTerminationKind::NONE};
    std::uint8_t reserved[3]{};
    std::int32_t exit_code{0},signal_number{0},runtime_error{0};
};
struct SupervisorFailureDiagnostic {
    std::uint64_t sequence{0},monotonic_ns{0},generation{0};
    char element_name[128]{};
    std::int32_t pid{-1};
    std::uint8_t exit_info_available{0},runtime_error_observed{0};
    ElementFailureKind failure_kind{ElementFailureKind::NONE};
    ProcessTerminationKind termination_kind{ProcessTerminationKind::NONE};
    std::int32_t exit_code{0},signal_number{0},runtime_error{0};
    std::uint32_t reserved_tail{0}; // avoid implicit tail padding (8-byte aligned)
};
struct SupervisorDiagnosticsInfo {
    std::uint32_t protocol_version{1},struct_size{sizeof(SupervisorDiagnosticsInfo)};
    std::int32_t pid{-1};
    std::uint32_t reserved{0};
    std::uint64_t process_start_ticks{0},revision{0},generation{0},last_error_sequence{0};
    ApplicationState application_state{ApplicationState::INITIALIZING};
    std::uint8_t reserved_state[3]{};
    std::uint32_t element_count{0},history_count{0},reserved_counts{0};
    // Oldest retained entry first; history_count<=64.
    SupervisorElementDiagnostic elements[MAX_SUPERVISOR_ELEMENTS]{};
    SupervisorFailureDiagnostic history[MAX_SUPERVISOR_DIAGNOSTIC_HISTORY]{};
};
static_assert(std::is_trivially_copyable_v<SupervisorElementDiagnostic>);
static_assert(std::is_trivially_copyable_v<SupervisorFailureDiagnostic>);
static_assert(std::is_trivially_copyable_v<SupervisorDiagnosticsInfo>);
static_assert(std::is_standard_layout_v<SupervisorDiagnosticsInfo>);
static_assert(sizeof(SupervisorElementDiagnostic)==152);
static_assert(sizeof(SupervisorFailureDiagnostic)==176);
static_assert(offsetof(SupervisorDiagnosticsInfo,elements)==64);
static_assert(sizeof(SupervisorDiagnosticsInfo)==64+MAX_SUPERVISOR_ELEMENTS*152+MAX_SUPERVISOR_DIAGNOSTIC_HISTORY*176);
}
