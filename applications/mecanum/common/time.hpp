#pragma once

#include <cerrno>
#include <cstdint>
#include <time.h>

namespace mecanum::common
{

inline int GetMonotonicTimestampUs(std::uint64_t& timestamp_us) noexcept
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -errno;
    timestamp_us = static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
                   static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
    return 0;
}

inline bool IsTimestampFresh(std::uint64_t sample_timestamp_us,
                             std::uint64_t now_timestamp_us,
                             std::uint64_t max_age_us) noexcept
{
    if (sample_timestamp_us == 0 || sample_timestamp_us > now_timestamp_us) return false;
    return now_timestamp_us - sample_timestamp_us <= max_age_us;
}

} // namespace mecanum::common
