#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace kcf {
// Caller-owned byte copy. Topic uses a 64-bit transport sequence; Parameter
// uses its uint64_t version. Set ignores sequence (not compare-and-swap).
struct DynamicPayload {
    std::uint64_t type_id{0};
    std::uint64_t sequence{0};
    std::vector<std::byte> bytes;
};
}
