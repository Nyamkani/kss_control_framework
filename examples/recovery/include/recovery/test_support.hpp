#pragma once
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "kcf/ipc/detail/recovery.hpp"

// White-box format-v2 fixtures are test-only. No production crash/sleep hooks.
namespace recovery_test
{
inline void Reap(pid_t pid)
{
    int status;assert(waitpid(pid,&status,0)==pid);
    assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
}
inline std::size_t Fds()
{
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});
}
template<class Storage> class Mapping
{
public:
    Storage* p;
    explicit Mapping(const std::string& name)
    {
        int fd=shm_open(name.c_str(),O_RDWR,0600);assert(fd>=0);
        struct stat st{};assert(fstat(fd,&st)==0&&st.st_size==sizeof(Storage));
        p=static_cast<Storage*>(mmap(nullptr,sizeof(Storage),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
        assert(p!=MAP_FAILED);close(fd);
    }
    ~Mapping(){munmap(p,sizeof(Storage));}
};
template<class Function> void KillLocked(Function function)
{
    int ready[2];assert(pipe(ready)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){close(ready[0]);function();assert(write(ready[1],"x",1)==1);for(;;)pause();}
    close(ready[1]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
    assert(kill(child,SIGKILL)==0);Reap(child);
}
inline void NoObject(const std::string& name)
{
    int fd=shm_open(name.c_str(),O_RDWR,0600);assert(fd<0&&errno==ENOENT);
}
// A creator paused with an incomplete header is protected while its flock is
// held. After SIGKILL/reap the same bytes may be reclaimed.
template<class Create> void Incomplete(const std::string& name, Create create, bool header_written = false)
{
    int ready[2];assert(pipe(ready)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(ready[0]);int fd=shm_open(name.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);
        assert(flock(fd,LOCK_EX)==0);
        if (header_written)
        {
            const kcf::detail::OwnerHeader header{0,0,0,2,0,static_cast<std::int32_t>(getpid()),0};
            assert(pwrite(fd,&header,sizeof(header),0)==sizeof(header));
        }
        assert(write(ready[1],"x",1)==1);for(;;)pause();
    }
    close(ready[1]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
    assert(create()==-EEXIST);
    kill(child,SIGKILL);Reap(child);
    assert(create()==0);
}
}
