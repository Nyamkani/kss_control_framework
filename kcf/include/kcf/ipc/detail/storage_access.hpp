#pragma once
#include "kcf/ipc/detail/storage_layout.hpp"
#include <cerrno>
#include <cstring>
#include <limits>
#include <thread>
namespace kcf::detail {
// Views contain process-local pointers only. Both typed and dynamic APIs use
// these exact transaction, recovery and notification operations.
struct ParameterView {
    pthread_mutex_t& mutex; pthread_cond_t& condition;
    std::uint64_t& version; std::uint64_t& previous_version;
    std::uint32_t& active_index; std::uint32_t& previous_active;
    std::atomic<std::uint32_t>& transaction_active;
    unsigned char* values; std::size_t size;
};
template<class T> ParameterView View(ParameterStorage<T>& s) {
    return {s.mutex,s.condition,s.version,s.previous_version,s.active_index,s.previous_active,
            s.transaction_active,reinterpret_cast<unsigned char*>(&s.value_slots),sizeof(T)};
}
inline int RecoverParameter(ParameterView s,int result) {
    if(result!=EOWNERDEAD)return result;
    if(s.transaction_active.load()) {
        s.active_index=s.previous_active;s.version=s.previous_version;s.transaction_active.store(0);
    }
    result=pthread_mutex_consistent(&s.mutex);
    if(result!=0)pthread_mutex_unlock(&s.mutex);
    return result;
}
inline int GetParameter(ParameterView s,void* value,std::uint64_t* version) {
    int result=RecoverParameter(s,pthread_mutex_lock(&s.mutex));
    if(result)return -result;
    if(s.active_index>1){pthread_mutex_unlock(&s.mutex);return -EPROTO;}
    std::memcpy(value,s.values+s.active_index*s.size,s.size);
    if(version)*version=s.version;
    return -pthread_mutex_unlock(&s.mutex);
}
inline int SetParameter(ParameterView s,const void* value) {
    int result=RecoverParameter(s,pthread_mutex_lock(&s.mutex));
    if(result)return -result;
    if(s.active_index>1){pthread_mutex_unlock(&s.mutex);return -EPROTO;}
    if(s.version==std::numeric_limits<std::uint64_t>::max()) {
        pthread_mutex_unlock(&s.mutex);return -EOVERFLOW;
    }
    s.previous_active=s.active_index;s.previous_version=s.version;
    s.transaction_active.store(1);std::atomic_thread_fence(std::memory_order_seq_cst);
    const auto inactive=1u-s.active_index;
    std::memcpy(s.values+inactive*s.size,value,s.size);
    s.active_index=inactive;s.version=s.previous_version+1;s.transaction_active.store(0);
    result=pthread_mutex_unlock(&s.mutex);
    if(result)return -result;
    return -pthread_cond_broadcast(&s.condition);
}
struct ChannelSlotView {
    std::atomic<std::uint32_t>& users; std::atomic<std::uint32_t>& version;
    std::uint32_t& sequence; unsigned char* data;
};
// One attempt. Retry is controlled by the reader; never touches notification.
// Pinning excludes the writer during non-atomic memcpy; a version-only check
// would not prevent a C++ data race. A dead reader can leave this pin behind.
inline int CopyChannelSnapshot(std::atomic<std::uint32_t>& index,
                               std::atomic<std::uint32_t>& sequence,
                               ChannelSlotView slot, std::uint32_t selected,
                               void* output,std::size_t size,std::uint32_t& copied) {
    constexpr auto writing=0x80000000u;
    auto users=slot.users.load();
    if(users>=writing-1 || !slot.users.compare_exchange_weak(users,users+1))return -EAGAIN;
    const auto before=slot.version.load();
    const auto published=sequence.load();
    if((before&1u) || index.load()!=selected || slot.sequence!=published) {
        slot.users.fetch_sub(1);return -EAGAIN;
    }
    std::memcpy(output,slot.data,size);
    const auto value_sequence=slot.sequence;
    const auto after=slot.version.load();slot.users.fetch_sub(1);
    if(before!=after || (after&1u))return -EAGAIN;
    copied=value_sequence;return 0;
}
}
