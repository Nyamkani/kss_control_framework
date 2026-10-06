#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <pthread.h>
namespace kcf::detail {
inline constexpr std::uint32_t STORAGE_FORMAT = 3;
inline constexpr std::uint64_t TOPIC_MAGIC = 0x4b4346545249504cULL;
inline constexpr std::uint64_t PARAMETER_MAGIC = 0x4b4346504152414dULL;
// Parameter retains format 3; Topic has its own incompatible queue layout.
inline constexpr std::uint32_t TOPIC_STORAGE_FORMAT = 4;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct ChannelHeader
{
    std::uint64_t magic{0}, size{0}, alignment{0};
    std::uint32_t format{TOPIC_STORAGE_FORMAT}, initialized{0};
    std::int32_t owner_pid{0};
    std::uint32_t reserved{0};
    std::uint64_t type_id{0}, layout_id{0};
    std::uint32_t depth{1}, reserved1{0};
    std::atomic<std::uint64_t> publish_sequence{0};
    pthread_mutex_t notify_mutex;
    pthread_cond_t notify_cond;
    std::uint64_t notify_sequence{0};
};
template<class T> struct ChannelSlot
{
    std::atomic<std::uint32_t> users{0};
    std::atomic<std::uint32_t> version{0};
    std::uint64_t sequence{0}; // protected by users
    alignas(T) unsigned char data[sizeof(T)];
};
// Shared by typed/dynamic readers. Each logical ring entry has three physical
// slots. Offsets/length use checked arithmetic before mmap or pointer access.
struct ChannelLayout
{
    std::size_t slots{0}, data{0}, stride{0}, length{0};
};
inline bool ComputeChannelLayout(std::size_t size, std::size_t alignment,
                                 std::uint32_t depth, ChannelLayout& out)
{
    if (!depth || !size || !alignment || (alignment & (alignment-1))) return false;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto align = [&](std::size_t n, std::size_t a, std::size_t& result) {
        if (n > maximum-(a-1)) return false;
        result = (n+a-1)&~(a-1); return true;
    };
    const auto slot_alignment = alignment > alignof(ChannelSlot<std::byte>) ? alignment : alignof(ChannelSlot<std::byte>);
    ChannelLayout value;
    if (!align(sizeof(ChannelHeader), slot_alignment, value.slots) ||
        !align(offsetof(ChannelSlot<std::byte>,data), alignment, value.data) ||
        size > maximum-value.data || !align(value.data+size, slot_alignment, value.stride)) return false;
    if (depth > (maximum-value.slots)/value.stride/3) return false;
    value.length = value.slots + static_cast<std::size_t>(depth)*3*value.stride;
    out=value; return true;
}
template<class T> struct ParameterStorage
    {
        std::uint64_t magic{0};
        std::uint64_t payload_size{sizeof(T)};
        std::uint64_t payload_alignment{alignof(T)};
        std::uint32_t format{STORAGE_FORMAT};
        std::uint32_t initialized{0};
        std::int32_t owner_pid{0};
        std::uint32_t reserved{0};
        std::uint64_t type_id{0}, layout_id{0};
        pthread_mutex_t mutex;
        pthread_cond_t condition;
        std::uint64_t version{0};
        std::uint64_t previous_version{0};
        std::uint32_t active_index{0}, previous_active{0};
        std::atomic<std::uint32_t> transaction_active{0};
        alignas(T) unsigned char value_slots[2][sizeof(T)];
    };

}
