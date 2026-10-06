#pragma once
#include "kcf/ipc/detail/storage_layout.hpp"
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

namespace kcf::detail
{
// PID reuse/EPERM/unknown errors conservatively prevent reclaim. Reap old
// children before recovery; this is lifecycle metadata, not authentication.
inline bool OwnerDead(std::int32_t pid)
{
    return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH;
}
struct OwnerHeader
{
    std::uint64_t magic, size, alignment;
    std::uint32_t format, initialized;
    std::int32_t owner_pid;
    std::uint32_t reserved;
    std::uint64_t type_id{0}, layout_id{0};
};
static_assert(sizeof(OwnerHeader) == 56);

// Serialize only name creation/reclaim and initial flock acquisition. The
// /dev/shm directory lock closes the shm_open -> flock gap without creating a
// persistent lock file. All KCF creators use it; initialization keeps fd flock.
// Reclaim requires old-generation peers stopped by the application. No reconnect.
inline int CreateOwnedShm(const char* name, std::uint64_t magic,
                          std::uint64_t size, std::uint64_t alignment, bool nonblocking = false,
                          std::uint32_t format = STORAGE_FORMAT)
{
    const int directory = open("/dev/shm", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -errno;
    if (flock(directory, LOCK_EX | (nonblocking ? LOCK_NB : 0)) != 0) { int e=errno; close(directory); return -e; }
    int result = -EEXIST;
    for (int attempt=0; attempt<2; ++attempt)
    {
        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
        {
            if (flock(fd, LOCK_EX | (nonblocking ? LOCK_NB : 0)) != 0)
            { result=-errno; shm_unlink(name); close(fd); break; }
            const OwnerHeader header{magic,size,alignment,format,0,static_cast<std::int32_t>(getpid()),0};
            const auto written = pwrite(fd,&header,sizeof(header),0);
            if (written != sizeof(header))
            { result=written<0?-errno:-EIO; shm_unlink(name); close(fd); break; }
            result=fd; // initialization flock remains held by returned descriptor
            break;
        }
        if (errno != EEXIST) { result=-errno; break; }
        fd=shm_open(name,O_RDWR,0600);
        if (fd<0) { result=-errno; break; }
        if (flock(fd,LOCK_EX | LOCK_NB)!=0) { close(fd); result=-EEXIST; break; }
        OwnerHeader old{};
        const auto count=pread(fd,&old,sizeof(old),0);
        bool reclaim=false;
        if (count>=0 && count<static_cast<ssize_t>(sizeof(old))) reclaim=true;
        else if (count==sizeof(old))
        {
            // No format migration. Unknown complete headers are never deleted.
            if (old.initialized==0)
                reclaim=old.owner_pid<=0 || OwnerDead(old.owner_pid);
            else if (old.initialized==1 && old.magic==magic && old.format==format && old.owner_pid>0)
                reclaim=OwnerDead(old.owner_pid);
        }
        if (!reclaim) { close(fd); result=-EEXIST; break; }
        if (shm_unlink(name)!=0) { result=-errno; close(fd); break; }
        close(fd);
    }
    flock(directory,LOCK_UN);
    close(directory);
    return result;
}
// Timed waits allow an already sleeping peer to notice owner death even if
// the dead locker never broadcast. This is not a heartbeat or reconnect.
inline timespec RecoveryDeadline()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC,&time);
    time.tv_nsec += 250000000;
    if (time.tv_nsec>=1000000000) { ++time.tv_sec; time.tv_nsec-=1000000000; }
    return time;
}
}
