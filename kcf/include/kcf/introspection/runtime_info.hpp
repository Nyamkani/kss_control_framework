#pragma once
#include "kcf/process/execution_mode.hpp"
#include "kcf/process/lifecycle.hpp"
#include <cstdint>
#include <type_traits>

namespace kcf
{
// Observational snapshot, not an identity credential or live heartbeat.
struct RuntimeInfo
{
    std::uint32_t protocol_version{1};
    std::uint32_t struct_size{sizeof(RuntimeInfo)};
    std::int32_t pid{0};
    std::uint64_t process_start_ticks{0};
    ExecutionMode execution_mode{ExecutionMode::STANDALONE};
    ProcessState state{ProcessState::STOPPED};
    std::int32_t runtime_error{0};
    char executable[128]{};
};
static_assert(std::is_trivially_copyable_v<RuntimeInfo>);
static_assert(std::is_standard_layout_v<RuntimeInfo>);
}
