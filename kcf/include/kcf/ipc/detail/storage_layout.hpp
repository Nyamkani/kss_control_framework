#pragma once
#include <atomic>
#include <cstdint>
#include <pthread.h>
namespace kcf::detail {
inline constexpr std::uint32_t STORAGE_FORMAT = 3;
inline constexpr std::uint64_t TOPIC_MAGIC = 0x4b4346545249504cULL;
inline constexpr std::uint64_t PARAMETER_MAGIC = 0x4b4346504152414dULL;
template<class T> struct ChannelSlot
    {
        std::atomic<std::uint32_t> users{0}; // writer bit or reader count
        std::atomic<std::uint32_t> version{0};
        std::uint32_t sequence{0}; // protected by users, belongs to this payload
        alignas(T) unsigned char data[sizeof(T)];
    };
template<class T> struct ChannelStorage
    {
        std::uint64_t magic{0};
        std::uint64_t size{sizeof(T)};
        std::uint64_t alignment{alignof(T)};
        std::uint32_t format{STORAGE_FORMAT};
        std::uint32_t initialized{0}; // read/written under initialization flock
        std::int32_t owner_pid{0};
        std::uint32_t reserved{0};
        std::uint64_t type_id{0}, layout_id{0};
        std::atomic<std::uint32_t> published_index{3}; // no snapshot yet
        std::atomic<std::uint32_t> publish_sequence{0};
        ChannelSlot<T> slots[3];
        pthread_mutex_t notify_mutex;
        pthread_cond_t notify_cond;
        std::uint32_t notify_sequence{0}; // only under notify_mutex
    };
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
