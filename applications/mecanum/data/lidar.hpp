#pragma once

#include "common.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mecanum::data
{

// PointT must contain no raw pointers or heap ownership.
template <typename PointT, std::size_t MaxPoints>
struct FixedLidarScan
{
    static_assert(MaxPoints > 0);
    static_assert(std::is_trivially_copyable_v<PointT>);
    static_assert(std::is_standard_layout_v<PointT>);

    SampleHeader header;

    std::uint32_t count{0};
    std::uint32_t reserved{0};

    PointT points[MaxPoints]{};
};

} // namespace mecanum::data
