#pragma once
#include <cstdint>
namespace kcf
{
enum class TopicStartPosition { NEXT, OLDEST };
struct TopicReadInfo
{
    std::uint64_t sequence{0};
    std::uint64_t missed{0}; // overwritten since this cursor's next expected item
};
}
