#pragma once
#include "kcf/ipc/detail/recovery.hpp"

#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kcf
{
// Low-frequency configuration state, independent of Topic. Peers require the
// same T/ABI; T must contain no pointers or heap ownership (not reflectable).
// Get/Set/Wait may overlap. Close requires all local operations to finish first.
template <typename T>
class SharedParameter
{
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(!std::is_pointer_v<T>);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    struct Storage
    {
        std::uint64_t magic{0};
        std::uint64_t payload_size{sizeof(T)};
        std::uint64_t payload_alignment{alignof(T)};
        std::uint32_t format{2};
        std::uint32_t initialized{0};
        std::int32_t owner_pid{0};
        std::uint32_t reserved{0};
        pthread_mutex_t mutex;
        pthread_cond_t condition;
        std::uint64_t version{0};
        std::uint64_t previous_version{0};
        std::uint32_t active_index{0}, previous_active{0};
        std::atomic<std::uint32_t> transaction_active{0};
        alignas(T) unsigned char value_slots[2][sizeof(T)];
    };

public:
    SharedParameter() = default;
    ~SharedParameter() { Close(); }
    SharedParameter(const SharedParameter&) = delete;
    SharedParameter& operator=(const SharedParameter&) = delete;

    int Create(const std::string& name, const T& initial_value)
    {
        if (fd_ >= 0 || owns_name_) return -EBUSY;
        int result = SetName(name);
        if (result != 0) return result;
        fd_ = detail::CreateOwnedShm(name_.c_str(), magic_, sizeof(T), alignof(T));
        if (fd_ < 0) { const int error = fd_; fd_ = -1; return error; }
        owns_name_ = true;
        if (ftruncate(fd_, sizeof(Storage)) != 0) return FailCreate(-errno);
        result = Map();
        if (result != 0) return FailCreate(result);
        new (storage_) Storage;
        storage_->owner_pid = getpid();
        result = InitializeSync();
        if (result != 0) return FailCreate(result);
        std::memcpy(storage_->value_slots[0], &initial_value, sizeof(T));
        storage_->magic = magic_;
        storage_->initialized = 1; // last, published by releasing initialization flock
        if (flock(fd_, LOCK_UN) != 0) return FailCreate(-errno);
        return 0;
    }

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
        if (pread(fd_, &header, sizeof(header), 0) == sizeof(header) &&
            header.magic == magic_ && header.format == 2 && header.initialized == 1 &&
            header.size == sizeof(T) && header.alignment == alignof(T) &&
            detail::OwnerDead(header.owner_pid)) return FailOpen(-EAGAIN);
        // If Open wins the lock before Create, zero size returns EAGAIN.
        if (flock(fd_, LOCK_SH) != 0) return FailOpen(-errno);
        struct stat info {};
        if (fstat(fd_, &info) != 0) return FailOpen(-errno);
        if (info.st_size == 0) return FailOpen(-EAGAIN);
        if (info.st_size != static_cast<off_t>(sizeof(Storage))) return FailOpen(-EMSGSIZE);
        result = Map();
        if (result != 0) return FailOpen(result);
        if (storage_->owner_pid <= 0) return FailOpen(-EPROTO);
        if (storage_->initialized != 1) return FailOpen(-EAGAIN);
        if (storage_->magic != magic_ || storage_->format != 2) return FailOpen(-EPROTO);
        if (storage_->payload_size != sizeof(T) || storage_->payload_alignment != alignof(T))
            return FailOpen(-EMSGSIZE);
        // A new generation must not attach to a dead owner's stale mapping
        // before the replacement owner has completed Create. Existing peers
        // retain their mappings; this is not hot reconnect or payload health.
        if (detail::OwnerDead(storage_->owner_pid)) return FailOpen(-EAGAIN);
        if (flock(fd_, LOCK_UN) != 0) return FailOpen(-errno);
        return 0;
    }

    // Optional version is copied under the SAME lock as the value.
    int Get(T& value, std::uint64_t* version = nullptr)
    {
        if (!storage_) return -EBADF;
        const int result = RecoverLock(pthread_mutex_lock(&storage_->mutex));
        if (result != 0) return -result;
        std::memcpy(&value, storage_->value_slots[storage_->active_index], sizeof(T));
        if (version) *version = storage_->version;
        return -pthread_mutex_unlock(&storage_->mutex);
    }

    int Set(const T& value)
    {
        if (!storage_) return -EBADF;
        int result = RecoverLock(pthread_mutex_lock(&storage_->mutex));
        if (result != 0) return -result;
        if (storage_->version == std::numeric_limits<std::uint64_t>::max())
        {
            pthread_mutex_unlock(&storage_->mutex);
            return -EOVERFLOW;
        }
        storage_->previous_active = storage_->active_index;
        storage_->previous_version = storage_->version;
        storage_->transaction_active.store(1);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto inactive = 1u - storage_->active_index;
        std::memcpy(storage_->value_slots[inactive], &value, sizeof(T));
        storage_->active_index = inactive;
        storage_->version = storage_->previous_version + 1;
        storage_->transaction_active.store(0); // commit: all new bytes precede this store
        result = pthread_mutex_unlock(&storage_->mutex);
        if (result != 0) return -result;
        return -pthread_cond_broadcast(&storage_->condition);
    }

    int Wait(T& value, std::uint64_t& last_version)
    {
        if (!storage_) return -EBADF;
        int result = RecoverLock(pthread_mutex_lock(&storage_->mutex));
        if (result != 0) return -result;
        while (!stopped_ && storage_->version == last_version)
        {
            const auto deadline = detail::RecoveryDeadline();
            result = pthread_cond_timedwait(&storage_->condition, &storage_->mutex, &deadline);
            if (result == ETIMEDOUT) result = 0;
            if (result == EOWNERDEAD) result = RecoverLock(result);
            else if (result != 0 && result != ENOTRECOVERABLE)
                pthread_mutex_unlock(&storage_->mutex);
            if (result != 0) return -result;
        }
        if (stopped_)
        {
            pthread_mutex_unlock(&storage_->mutex);
            return -ECANCELED;
        }
        std::memcpy(&value, storage_->value_slots[storage_->active_index], sizeof(T));
        last_version = storage_->version;
        return -pthread_mutex_unlock(&storage_->mutex);
    }

    int StopWait()
    {
        if (!storage_) return -EBADF;
        int result = RecoverLock(pthread_mutex_lock(&storage_->mutex));
        if (result != 0) return -result;
        stopped_ = true; // local predicate, synchronized with Wait's mutex
        result = pthread_cond_broadcast(&storage_->condition);
        const int unlocked = pthread_mutex_unlock(&storage_->mutex);
        return -(result != 0 ? result : unlocked);
    }

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
        stopped_ = false;
        return result;
    }

    // Owner only, explicitly invoked; works after Close. Do not destroy shared
    // pthread objects while other processes may still have open mappings.
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
        if (name.size() < 2 || name.front() != '/' || name.size() > 240 ||
            name.find('\0') != std::string::npos) return -EINVAL;
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
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || alignof(Storage) > static_cast<std::size_t>(page_size)) return -EINVAL;
        void* address = mmap(nullptr, sizeof(Storage), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (address == MAP_FAILED) return -errno;
        storage_ = static_cast<Storage*>(address);
        return 0;
    }

    int InitializeSync()
    {
        pthread_mutexattr_t mutex_attr;
        int result = pthread_mutexattr_init(&mutex_attr);
        if (result != 0) return -result;
        result = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        if (result == 0) result = pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
        if (result == 0) result = pthread_mutex_init(&storage_->mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);
        if (result != 0) return -result;
        pthread_condattr_t cond_attr;
        result = pthread_condattr_init(&cond_attr);
        if (result == 0)
        {
            result = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
            if (result == 0) result = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
            if (result == 0) result = pthread_cond_init(&storage_->condition, &cond_attr);
            pthread_condattr_destroy(&cond_attr);
        }
        if (result != 0) pthread_mutex_destroy(&storage_->mutex);
        return -result;
    }
    int RecoverLock(int result)
    {
        if (result != EOWNERDEAD) return result;
        if (storage_->transaction_active.load())
        {
            storage_->active_index = storage_->previous_active;
            storage_->version = storage_->previous_version;
            storage_->transaction_active.store(0);
        }
        result = pthread_mutex_consistent(&storage_->mutex);
        if (result != 0) pthread_mutex_unlock(&storage_->mutex);
        return result;
    }
    int FailOpen(int error) { Close(); return error; }
    int FailCreate(int error) { Unlink(); Close(); return error; }
    static constexpr std::uint64_t magic_ = 0x4b4346504152414dULL;
    Storage* storage_{nullptr};
    int fd_{-1};
    std::string name_;
    bool owns_name_{false};
    bool stopped_{false};
};
} // namespace kcf
