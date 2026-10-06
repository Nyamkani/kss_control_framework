#pragma once

#include <cstdint>
#include <type_traits>

struct TestMessage
{
    std::uint64_t count{0};
    double value{0.0};
};

static_assert(std::is_trivially_copyable_v<TestMessage>);
