#pragma once

#include <cstdint>
#include <type_traits>

namespace mecanum::data
{

struct SampleHeader
{
    // Producer sample number, independent of the KCF Topic sequence.
    std::uint64_t sequence{0};
    // CLOCK_MONOTONIC-based microseconds when the application sample is finalized.
    std::uint64_t timestamp_us{0};
};

static_assert(std::is_trivially_copyable_v<SampleHeader>);
static_assert(std::is_standard_layout_v<SampleHeader>);

} // namespace mecanum::data
