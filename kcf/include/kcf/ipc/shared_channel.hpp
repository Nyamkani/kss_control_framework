#pragma once
#include "kcf/ipc/detail/recovery.hpp"
#include "kcf/ipc/detail/storage_access.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"

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

// Linux/POSIX, one publishing thread and one reader per local channel object.
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
    using Storage = detail::ChannelStorage<T>;

public:
    SharedChannel() = default;
    ~SharedChannel() { Close(); }
    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;

    int Create(const std::string& name) { return CreateImpl(name, false); }
    // No waiting on the discovery-directory flock; failure is retryable.
    int TryCreate(const std::string& name) { return CreateImpl(name, true); }

private:
    int CreateImpl(const std::string& name, bool nonblocking)
    {
        if (fd_ >= 0 || owns_name_) return -EBUSY;
        int result = SetName(name);
        if (result != 0) return result;
        fd_ = detail::CreateOwnedShm(name_.c_str(), magic, sizeof(T), alignof(T), nonblocking);
        if (fd_ < 0) { const int error = fd_; fd_ = -1; return error; }
        owns_name_ = true;
        if (ftruncate(fd_, sizeof(Storage)) != 0) return FailCreate(-errno);
        result = Map();
        if (result != 0) return FailCreate(result);
        // Explicitly start atomic object lifetimes before exposing metadata.
        new (storage_) Storage;
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
    int Open(const std::string& name)
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
            header.magic == magic && header.format == detail::STORAGE_FORMAT && header.initialized == 1 &&
            header.size == sizeof(T) && header.alignment == alignof(T) &&
            detail::OwnerDead(header.owner_pid)) return FailOpen(-EAGAIN);
        // If Open wins before the creator's flock, zero size yields EAGAIN.
        // No pthread or atomic object is used until initialization is complete.
        if (flock(fd_, LOCK_SH) != 0) return FailOpen(-errno);
        struct stat info {};
        if (fstat(fd_, &info) != 0) return FailOpen(-errno);
        if (info.st_size == 0) return FailOpen(-EAGAIN);
        if (info.st_size != static_cast<off_t>(sizeof(Storage))) return FailOpen(-EPROTO);
        result = Map();
        if (result != 0) return FailOpen(result);
        if (storage_->owner_pid <= 0) return FailOpen(-EPROTO);
        if (storage_->initialized != 1) return FailOpen(-EAGAIN);
        if (storage_->magic != magic || storage_->format != detail::STORAGE_FORMAT ||
            storage_->size != sizeof(T) || storage_->alignment != alignof(T))
            return FailOpen(-EPROTO);
        // A new generation must not attach to a dead owner's stale mapping
        // before the replacement owner has completed Create. Existing peers
        // retain their mappings; this is not hot reconnect or payload health.
        if (detail::OwnerDead(storage_->owner_pid)) return FailOpen(-EAGAIN);
        if (flock(fd_, LOCK_UN) != 0) return FailOpen(-errno);
        return 0;
    }

    int Publish(const T& value)
    {
        if (!storage_) return -EBADF;
        if (!publisher_) return -EPERM;
        const auto current = storage_->published_index.load();
        std::uint32_t target = 3;
        for (std::uint32_t offset = 1; offset <= 3; ++offset)
        {
            const auto index = (current + offset) % 3;
            if (index == current) continue;
            std::uint32_t free = 0;
            if (storage_->slots[index].users.compare_exchange_strong(free, writing))
            {
                target = index;
                break;
            }
        }
        // Never overwrite a reader's non-atomic memcpy or wait indefinitely.
        // Caller may retry when both non-published slots are still being copied.
        if (target == 3) return -EAGAIN;
        Slot& slot = storage_->slots[target];
        slot.version.fetch_add(1); // odd: write in progress
        std::memcpy(slot.data, &value, sizeof(T));
        const auto sequence = storage_->publish_sequence.load() + 1u;
        slot.sequence = sequence;
        slot.version.fetch_add(1); // even: complete
        slot.users.store(0);
        storage_->published_index.store(target);
        storage_->publish_sequence.store(sequence);

        // Payload is complete before entering notification's critical section.
        int result = RecoverLock(pthread_mutex_lock(&storage_->notify_mutex));
        if (result != 0) return -result;
        storage_->notify_sequence = sequence;
        result = pthread_mutex_unlock(&storage_->notify_mutex);
        if (result != 0) return -result;
        return -pthread_cond_broadcast(&storage_->notify_cond);
    }

    // Polling-only snapshot publication. No notification mutex/condition,
    // allocation or reader wait. At most three slot CAS attempts; EAGAIN if
    // readers pin all usable slots. Use ReadLatestSnapshot, not Wait, to observe.
    // Same single-publisher-thread contract as Publish. Do not mix modes.
    int TryPublishSnapshot(const T& value)
    {
        if (!storage_) return -EBADF;
        if (!publisher_) return -EPERM;
        const auto current = storage_->published_index.load();
        std::uint32_t target = 3;
        for (std::uint32_t offset = 1; offset <= 3; ++offset)
        {
            const auto index = (current + offset) % 3;
            if (index == current) continue;
            std::uint32_t free = 0;
            if (storage_->slots[index].users.compare_exchange_strong(free, writing))
            {
                target = index;
                break;
            }
        }
        // Never overwrite a reader's non-atomic memcpy or wait indefinitely.
        // Caller may retry when both non-published slots are still being copied.
        if (target == 3) return -EAGAIN;
        Slot& slot = storage_->slots[target];
        slot.version.fetch_add(1); // odd: write in progress
        std::memcpy(slot.data, &value, sizeof(T));
        const auto sequence = storage_->publish_sequence.load() + 1u;
        slot.sequence = sequence;
        slot.version.fetch_add(1); // even: complete
        slot.users.store(0);
        storage_->published_index.store(target);
        storage_->publish_sequence.store(sequence);

        return 0;
    }

    int Wait(std::uint32_t& last_notify_sequence)
    {
        if (!storage_) return -EBADF;
        int result = RecoverLock(pthread_mutex_lock(&storage_->notify_mutex));
        if (result != 0) return -result;
        while (!stopped_.load() && storage_->notify_sequence == last_notify_sequence)
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
        last_notify_sequence = storage_->notify_sequence;
        result = pthread_mutex_unlock(&storage_->notify_mutex);
        return stopped ? -ECANCELED : -result;
    }

    int ReadLatestSnapshot(T& value, std::uint32_t& sequence)
    {
        if (!storage_) return -EBADF;
        while (!stopped_.load())
        {
            const auto index = storage_->published_index.load();
            if (index >= 3) return -EAGAIN;
            Slot& slot = storage_->slots[index];
            const int result = detail::CopyChannelSnapshot(storage_->published_index,
                storage_->publish_sequence, {slot.users, slot.version, slot.sequence, slot.data},
                index, &value, sizeof(T), sequence);
            if (result == 0) return 0;
            std::this_thread::yield();
        }
        return -ECANCELED;
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
            if (munmap(storage_, sizeof(Storage)) != 0) result = -errno;
            storage_ = nullptr;
        }
        if (fd_ >= 0)
        {
            if (close(fd_) != 0 && result == 0) result = -errno;
            fd_ = -1;
        }
        publisher_ = false;
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
        if (page <= 0 || alignof(Storage) > static_cast<std::size_t>(page)) return -EINVAL;
        void* address = mmap(nullptr, sizeof(Storage), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
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
    std::string name_;
    bool owns_name_{false};
    bool publisher_{false};
    std::atomic<bool> stopped_{false};
};

} // namespace kcf
