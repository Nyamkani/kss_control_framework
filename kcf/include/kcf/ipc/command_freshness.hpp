#pragma once
#include "kcf/ipc/topic_read_info.hpp"
#include <cerrno>
#include <chrono>
#include <cstdint>

namespace kcf {
enum class CommandFreshnessState { INVALID, VALID, EXPIRED };

// Single control-thread ownership; no locks, allocation, I/O or command output.
// Age measures time since this receiver OBSERVED a new commit, not source age.
// Use ReadNext with NEXT (normally depth=1). Do not feed stored ReadLatest values
// on startup. Reset together with Close/Create on explicit reconnect; retained
// proxy bytes are not a new command. A backlog is NOT proof of source freshness.
template<class Clock = std::chrono::steady_clock>
class BasicCommandFreshnessMonitor {
public:
    static_assert(Clock::is_steady, "A monotonic clock is required");
    using Duration = typename Clock::duration;
    using TimePoint = typename Clock::time_point;
    struct Snapshot {
        CommandFreshnessState state{CommandFreshnessState::INVALID};
        bool has_observation{false};
        std::uint64_t sequence{0};
        TimePoint last_observed{};
        Duration age{};
    };
    // Nonpositive timeout is fail-closed: Observe never accepts a command.
    explicit BasicCommandFreshnessMonitor(Duration timeout = std::chrono::milliseconds(100)) noexcept
        : timeout_(timeout) {}
    void Reset() noexcept { sequence_=0;last_={};observed_=false;fault_=false; }
    // Storage/read errors latch INVALID until explicit Reset/reconnection.
    void Invalidate() noexcept { fault_=true; }
    bool Observe(std::uint64_t sequence, TimePoint now = Clock::now()) noexcept {
        if(fault_ || timeout_<=Duration::zero())return false;
        if(!sequence || (observed_ && (sequence<sequence_ || now<last_))){Invalidate();return false;}
        if(observed_ && sequence==sequence_)return false;
        sequence_=sequence;last_=now;observed_=true;return true;
    }
    // -EAGAIN is no new commit/temporary contention: retain the original deadline.
    // Every other read error requires caller recovery; never silently remain VALID.
    bool ObserveRead(int result, const TopicReadInfo& info, TimePoint now = Clock::now()) noexcept {
        if(result==-EAGAIN)return false;
        if(result){Invalidate();return false;}
        return Observe(info.sequence,now);
    }
    Snapshot GetSnapshot(TimePoint now = Clock::now()) const noexcept {
        Snapshot out;out.has_observation=observed_;out.sequence=sequence_;out.last_observed=last_;
        if(observed_ && now>=last_)out.age=now-last_;
        if(!observed_ || fault_ || timeout_<=Duration::zero() || now<last_)return out;
        out.state=out.age>=timeout_?CommandFreshnessState::EXPIRED:CommandFreshnessState::VALID;
        return out;
    }
private:
    Duration timeout_;
    TimePoint last_{};
    std::uint64_t sequence_{0};
    bool observed_{false},fault_{false};
};
using CommandFreshnessMonitor = BasicCommandFreshnessMonitor<>;
} // namespace kcf
