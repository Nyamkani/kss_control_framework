#pragma once
#include "kcf/system/application_state.hpp"
#include "kcf/system/element_failure_kind.hpp"
#include <cstddef>
#include <type_traits>
namespace kcf
{
inline constexpr char SYSTEM_STATUS_TOPIC[] = "/kcf/system/status";
enum class ProcessTerminationKind : std::uint8_t { NONE, EXITED, SIGNALED };
struct SystemStatus
{
    std::uint64_t sequence{0};
    ApplicationState state{ApplicationState::INITIALIZING};
    std::uint8_t error_valid{0};
    ProcessTerminationKind termination_kind{ProcessTerminationKind::NONE};
    ElementFailureKind failure_kind{ElementFailureKind::NONE};
    std::int32_t failed_pid{-1};
    std::int32_t exit_code{0};
    std::int32_t signal_number{0};
    std::int32_t runtime_error{0};
    std::uint32_t reserved0{0};
    char error_element[64]{}; // UTF-8 bytes, at most 63 bytes plus NUL
};
static_assert(std::is_trivially_copyable_v<SystemStatus>);
static_assert(std::is_standard_layout_v<SystemStatus>);
static_assert(sizeof(SystemStatus) == 96);
static_assert(offsetof(SystemStatus, failed_pid) == 12);
static_assert(offsetof(SystemStatus, error_element) == 32);
}
