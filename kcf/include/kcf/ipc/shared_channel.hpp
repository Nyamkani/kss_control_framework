#pragma once
#include "kcf/ipc/detail/recovery.hpp"
#include "kcf/ipc/detail/storage_access.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include "kcf/ipc/topic_read_info.hpp"
#include <limits>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kcf
{

// Linux/POSIX, one publishing thread. Serialize ReadNext calls per local object.
// Snapshot reads use no cursor and may overlap sequential reads. Finish all
// local operations before Close. No automatic reconnect to a replacement SHM.
// All processes must use the same T/layout/ABI. T must contain no pointers or
// heap ownership; trivially_copyable alone cannot inspect aggregate members.
template <typename T>
class SharedChannel
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(!std::is_pointer_v<T>);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "SharedChannel requires lock-free 32-bit atomics");

    static constexpr std::uint32_t writing = 0x80000000u;
    using Slot = detail::ChannelSlot<T>;
    using Storage = detail::ChannelHeader;

public:
    SharedChannel() = default;
    ~SharedChannel() { Close(); }
    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;

    int Create(const std::string& name, std::uint32_t depth = 1) { return CreateImpl(name, depth, false); }
    // No waiting on the discovery-directory flock; failure is retryable.
    int TryCreate(const std::string& name, std::uint32_t depth = 1) { return CreateImpl(name, depth, true); }

private:
    int CreateImpl(const std::string& name, std::uint32_t depth, bool nonblocking)
    {
        if (fd_ >= 0 || owns_name_) return -EBUSY;
        if (!detail::ComputeChannelLayout(sizeof(T), alignof(T), depth, layout_) ||
            layout_.length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return -EINVAL;
        int result = SetName(name);
        if (result != 0) return result;
        fd_ = detail::CreateOwnedShm(name_.c_str(), magic, sizeof(T), alignof(T), nonblocking, detail::TOPIC_STORAGE_FORMAT);
        if (fd_ < 0) { const int error = fd_; fd_ = -1; return error; }
        owns_name_ = true;
        if (ftruncate(fd_, layout_.length) != 0) return FailCreate(-errno);
        result = Map();
        if (result != 0) return FailCreate(result);
        // Explicitly start atomic object lifetimes before exposing metadata.
        new (storage_) Storage;
        storage_->size = sizeof(T); storage_->alignment = alignof(T); storage_->depth = depth;
        for (std::size_t i=0; i<static_cast<std::size_t>(depth)*3; ++i) new (&SlotAt(i)) Slot;
        storage_->owner_pid = getpid();
        const auto identity = detail::DeclaredStorageIdentity<T>();
        storage_->type_id = identity.type_id;
        storage_->layout_id = identity.layout_id;
        result = InitializeNotify();
        if (result != 0) return FailCreate(result);
        storage_->magic = magic;
        storage_->initialized = 1;
        if (flock(fd_, LOCK_UN) != 0) return FailCreate(-errno);
        publisher_ = true;
        return 0;
    }

public:
    int Open(const std::string& name, TopicStartPosition start = TopicStartPosition::NEXT)
    {
        if (fd_ >= 0 || owns_name_) return -EBUSY;
        int result = SetName(name);
        if (result != 0) return result;
        fd_ = shm_open(name_.c_str(), O_RDWR, 0600);
        if (fd_ < 0) return -errno;
        // Reject known stale objects before taking a shared initialization lock:
        // a retrying client must not block the replacement owner's exclusive lock.
        detail::OwnerHeader header{};
        const auto count = pread(fd_, &header, sizeof(header), 0);
        if (count < 0) return FailOpen(-errno);
        // Incomplete readers have nothing to attach to. Return without LOCK_SH
        // so recovery can acquire LOCK_EX. A live initializer is also safely
        // retried; this check neither infers owner death nor reclaims anything.
        if (count < static_cast<ssize_t>(sizeof(header)) || header.initialized == 0)
            return FailOpen(-EAGAIN);
        if (count == sizeof(header) &&
            header.magic == magic && header.format == detail::TOPIC_STORAGE_FORMAT && header.initialized == 1 &&
            header.size == sizeof(T) && header.alignment == alignof(T) &&
            detail::OwnerDead(header.owner_pid)) return FailOpen(-EAGAIN);
        // If Open wins before the creator's flock, zero size yields EAGAIN.
        // No pthread or atomic object is used until initialization is complete.
        if (flock(fd_, LOCK_SH) != 0) return FailOpen(-errno);
        struct stat info {};
        if (fstat(fd_, &info) != 0) return FailOpen(-errno);
        if (info.st_size == 0) return FailOpen(-EAGAIN);
        std::uint32_t depth=0;
        if (pread(fd_, &depth, sizeof(depth), offsetof(Storage,depth)) != sizeof(depth)) return FailOpen(-EPROTO);
        if (!detail::ComputeChannelLayout(sizeof(T), alignof(T), depth, layout_) ||
            info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != layout_.length) return FailOpen(-EPROTO);
        result = Map();
        if (result != 0) return FailOpen(result);
        if (storage_->depth != depth) return FailOpen(-EPROTO);
        if (storage_->owner_pid <= 0) return FailOpen(-EPROTO);
        if (storage_->initialized != 1) return FailOpen(-EAGAIN);
        if (storage_->magic != magic || storage_->format != detail::TOPIC_STORAGE_FORMAT ||
            storage_->size != sizeof(T) || storage_->alignment != alignof(T))
            return FailOpen(-EPROTO);
        // A new generation must not attach to a dead owner's stale mapping
        // before the replacement owner has completed Create. Existing peers
        // retain their mappings; this is not hot reconnect or payload health.
        if (detail::OwnerDead(storage_->owner_pid)) return FailOpen(-EAGAIN);
        if (flock(fd_, LOCK_UN) != 0) return FailOpen(-errno);
        const auto latest=storage_->publish_sequence.load();
        // Store the last consumed sequence, avoiding UINT64_MAX + 1 overflow.
        cursor_ = start == TopicStartPosition::NEXT ? latest : (latest > depth ? latest-depth : 0);
        return 0;
    }

    int Publish(const T& value) { return PublishImpl(value, true); }

    // Polling-only publication: no notify lock, allocation, or reader wait.
    // Do not mix this mode with Wait-based observation on the same channel.
    int TryPublishSnapshot(const T& value) { return PublishImpl(value, false); }

private:
    int PublishImpl(const T& value, bool notify)
    {
        if (!storage_) return -EBADF;
        if (!publisher_) return -EPERM;
        // Acquire/recover notification BEFORE mutation. A poisoned mutex must
        // not turn a failed Publish into a committed message.
        if (notify) {
            const int error=RecoverLock(pthread_mutex_lock(&storage_->notify_mutex));
            if (error) return -error;
        }
        const auto unlock=[&] { if (notify) pthread_mutex_unlock(&storage_->notify_mutex); };
        const auto previous=storage_->publish_sequence.load();
        if (previous==std::numeric_limits<std::uint64_t>::max()) { unlock(); return -EOVERFLOW; }
        const auto sequence=previous+1;
        const auto base=static_cast<std::size_t>((sequence-1)%storage_->depth)*3;
        const auto retained=sequence>storage_->depth ? sequence-storage_->depth : 0;
        Slot* target=nullptr;
        for (unsigned i=0; i<3; ++i) {
            Slot& slot=SlotAt(base+i);
            std::uint32_t free=0;
            if (!slot.users.compare_exchange_strong(free,writing)) continue;
            if (retained && slot.sequence==retained) { slot.users.store(0); continue; }
            target=&slot; break;
        }
        if (!target) { unlock(); return -EAGAIN; }
        // Waking under the mutex is safe: waiters cannot test the predicate
        // until commit and unlock. Check all fallible notification work first.
        if (notify) {
            const int error=pthread_cond_broadcast(&storage_->notify_cond);
            if (error) { target->users.store(0); unlock(); return -error; }
        }
        target->version.fetch_add(1);
        std::memcpy(target->data,&value,sizeof(T));
        target->sequence=sequence;
        target->version.fetch_add(1);
        target->users.store(0);
        storage_->publish_sequence.store(sequence); // single commit point
        if (notify) storage_->notify_sequence=sequence;
        // Mutex is valid and owned here. No post-commit failure return: callers
        // must never retry a payload that was already accepted.
        unlock();
        return 0;
    }

public:
    int Wait(std::uint64_t& last) { return WaitImpl(last); }
    // Legacy notification API exposes the low 32 bits only.
    int Wait(std::uint32_t& last) { return WaitImpl(last); }
    std::uint32_t GetDepth() const { return storage_ ? storage_->depth : 0; }

    int ReadLatest(T& value, TopicReadInfo& info)
    {
        if (!storage_) return -EBADF;
        if (stopped_.load()) return -ECANCELED;
        const auto sequence=storage_->publish_sequence.load();
        if (!sequence) return -EAGAIN;
        const int result=ReadSequence(value,sequence);
        if (!result) info={sequence,0};
        return result;
    }
    // Cursor changes only after a successful copy. ReadLatest is independent.
    int ReadNext(T& value, TopicReadInfo& info)
    {
        if (!storage_) return -EBADF;
        if (stopped_.load()) return -ECANCELED;
        const auto latest=storage_->publish_sequence.load();
        if (cursor_>=latest) return -EAGAIN;
        const auto oldest=latest>=storage_->depth ? latest-storage_->depth+1 : 1;
        const auto expected=cursor_+1;
        const auto wanted=expected<oldest ? oldest : expected;
        const int result=ReadSequence(value,wanted);
        if (!result) { info={wanted,wanted-expected}; cursor_=wanted; }
        return result;
    }
    int ReadLatestSnapshot(T& value, std::uint64_t& sequence)
    {
        if (!storage_) return -EBADF;
        while (!stopped_.load()) {
            TopicReadInfo info;
            const int result=ReadLatest(value,info);
            if (!result) { sequence=info.sequence; return 0; }
            if (result!=-EAGAIN || storage_->publish_sequence.load()==0) return result;
            std::this_thread::yield();
        }
        return -ECANCELED;
    }
    int ReadLatestSnapshot(T& value, std::uint32_t& sequence)
    {
        std::uint64_t wide=0;
        const int result=ReadLatestSnapshot(value,wide);
        if (!result) sequence=static_cast<std::uint32_t>(wide);
        return result;
    }

    int StopWait()
    {
        if (!storage_) return -EBADF;
        stopped_.store(true); // also stop a payload reader if the mutex is unrecoverable
        int result = RecoverLock(pthread_mutex_lock(&storage_->notify_mutex));
        if (result != 0) return -result;
        result = pthread_cond_broadcast(&storage_->notify_cond);
        const int unlocked = pthread_mutex_unlock(&storage_->notify_mutex);
        return -(result != 0 ? result : unlocked);
    }

    // Caller must finish/join all local operations first. No global deletion.
    int Close()
    {
        int result = 0;
        if (storage_)
        {
            if (munmap(storage_, layout_.length) != 0) result = -errno;
            storage_ = nullptr;
        }
        if (fd_ >= 0)
        {
            if (close(fd_) != 0 && result == 0) result = -errno;
            fd_ = -1;
        }
        publisher_ = false;
        cursor_ = 0;
        stopped_.store(false);
        return result;
    }

    // Explicit owner cleanup, also valid after Close. Existing readers retain
    // their mappings; never destroy shared pthread objects under those readers.
    int Unlink()
    {
        if (!owns_name_) return -EPERM;
        if (shm_unlink(name_.c_str()) != 0) return -errno;
        owns_name_ = false;
        return 0;
    }

private:
    Slot& SlotAt(std::size_t index) const
    {
        return *reinterpret_cast<Slot*>(reinterpret_cast<unsigned char*>(storage_)+layout_.slots+index*layout_.stride);
    }
    int ReadSequence(T& value, std::uint64_t sequence)
    {
        const auto base=static_cast<std::size_t>((sequence-1)%storage_->depth)*3;
        alignas(T) unsigned char copy[sizeof(T)];
        for (unsigned i=0; i<3; ++i) {
            Slot& slot=SlotAt(base+i);
            const int result=detail::CopyChannelSequence(storage_->publish_sequence,storage_->depth,
                {slot.users,slot.version,slot.sequence,slot.data},sequence,copy,sizeof(T));
            if (!result) { std::memcpy(&value,copy,sizeof(T)); return 0; }
        }
        return -EAGAIN;
    }
    int SetName(const std::string& name)
    {
        if (name.size() < 2 || name[0] != '/' || name.size() > 240 ||
            name.find('\0') != std::string::npos) return -EINVAL;
        // Preserve /kcf_test_topic. Encode nested logical names without aliases.
        name_ = "/";
        for (std::size_t i = 1; i < name.size(); ++i)
        {
            if (name[i] == '/') name_ += "%2F";
            else if (name[i] == '%') name_ += "%25";
            else name_ += name[i];
        }
        return name_.size() <= 250 ? 0 : -ENAMETOOLONG;
    }

    int Map()
    {
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0 || alignof(T) > static_cast<std::size_t>(page) || alignof(Storage) > static_cast<std::size_t>(page)) return -EINVAL;
        void* address = mmap(nullptr, layout_.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address == MAP_FAILED) return -errno;
        storage_ = static_cast<Storage*>(address);
        return 0;
    }

    int InitializeNotify()
    {
        pthread_mutexattr_t mutex_attr;
        int result = pthread_mutexattr_init(&mutex_attr);
        if (result != 0) return -result;
        result = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        if (result == 0) result = pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
        if (result == 0) result = pthread_mutex_init(&storage_->notify_mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);
        if (result != 0) return -result;
        pthread_condattr_t cond_attr;
        result = pthread_condattr_init(&cond_attr);
        if (result == 0)
        {
            result = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
            if (result == 0) result = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
            if (result == 0) result = pthread_cond_init(&storage_->notify_cond, &cond_attr);
            pthread_condattr_destroy(&cond_attr);
        }
        if (result != 0) pthread_mutex_destroy(&storage_->notify_mutex);
        return -result;
    }

    template<class Sequence>
    int WaitImpl(Sequence& last_notify_sequence)
    {
        if (!storage_) return -EBADF;
        int result = RecoverLock(pthread_mutex_lock(&storage_->notify_mutex));
        if (result != 0) return -result;
        while (!stopped_.load() && static_cast<Sequence>(storage_->notify_sequence) == last_notify_sequence)
        {
            const auto deadline = detail::RecoveryDeadline();
            result = pthread_cond_timedwait(&storage_->notify_cond, &storage_->notify_mutex, &deadline);
            if (result == ETIMEDOUT) result = 0;
            if (result == EOWNERDEAD) result = RecoverLock(result);
            else if (result != 0 && result != ENOTRECOVERABLE)
                pthread_mutex_unlock(&storage_->notify_mutex);
            if (result != 0) return -result;
        }
        const bool stopped = stopped_.load();
        last_notify_sequence = static_cast<Sequence>(storage_->notify_sequence);
        result = pthread_mutex_unlock(&storage_->notify_mutex);
        return stopped ? -ECANCELED : -result;
    }

    int RecoverLock(int result)
    {
        if (result != EOWNERDEAD) return result;
        storage_->notify_sequence = storage_->publish_sequence.load();
        result = pthread_mutex_consistent(&storage_->notify_mutex);
        if (result != 0) pthread_mutex_unlock(&storage_->notify_mutex);
        return result;
    }
    int FailOpen(int error) { Close(); return error; }
    int FailCreate(int error) { Unlink(); Close(); return error; }
    static constexpr std::uint64_t magic = 0x4b4346545249504cULL;
    int fd_{-1};
    Storage* storage_{nullptr};
    detail::ChannelLayout layout_{};
    std::uint64_t cursor_{0};
    std::string name_;
    bool owns_name_{false};
    bool publisher_{false};
    std::atomic<bool> stopped_{false};
};

} // namespace kcf
