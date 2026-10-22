#pragma once

#include <vector>

namespace kcf
{

enum class SchedulingPolicy
{
    INHERIT,
    OTHER,
    FIFO,
    RR
};

struct ExecutionProfile
{
    SchedulingPolicy policy{SchedulingPolicy::INHERIT};
    // INHERIT/OTHER require 0; FIFO/RR use the platform's priority range.
    int priority{0};
    // Empty preserves inherited affinity. Nonempty IDs must fit cpu_set_t and
    // the configured CPU range. Kernel permission/cpuset checks still apply.
    std::vector<int> cpu_affinity;
    // ProcessRuntime owns process-wide MCL_CURRENT|MCL_FUTURE locks from before
    // Setup through Shutdown/finalization. Opt-in; requires sufficient memlock limit.
    bool lock_memory{false};
};


// Internal launch handshake, not an application setting. Process replaces any
// inherited value; Runtime accepts only "0"/"1" and removes it before Setup.
namespace detail
{
inline constexpr char EXECUTION_LOCK_MEMORY_ENV[] = "KCF_EXECUTION_LOCK_MEMORY";
}

} // namespace kcf
