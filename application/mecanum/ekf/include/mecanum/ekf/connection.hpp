#pragma once
#include "kcf/ipc/subscriber.hpp"
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mecanum::ekf {
// KCF v5.1 has no public generation accessor/hot reconnect. Observe its POSIX
// object identity without editing storage; Subscriber still validates the ABI.
// Naming and OwnerHeader are intentionally version-coupled to actual v5.1 headers.
template<class T> class Connection {
public:
    explicit Connection(std::string name):name_(std::move(name)),shm_name_("/") {
        if (name_.size()<2 || name_[0]!='/' || name_.size()>240 || name_.find('\0')!=std::string::npos)
            throw std::invalid_argument("Invalid Topic name");
        for (std::size_t i=1;i<name_.size();++i)
            shm_name_+=name_[i]=='/' ? "%2F" : name_[i]=='%' ? "%25" : std::string(1,name_[i]);
        if (shm_name_.size()>250) throw std::invalid_argument("Topic name too long");
    }
    ~Connection() { subscriber.Close(); if (identity_fd_>=0) close(identity_fd_); }
    bool Refresh(unsigned depth) {
        const auto before_generation=generation;
        const bool before_connected=connected;
        const int probe=shm_open(shm_name_.c_str(),O_RDONLY|O_CLOEXEC,0600);
        if (probe<0) { last_error=-errno; Disconnect(); return before_connected; }
        struct stat identity{};
        kcf::detail::OwnerHeader header{};
        if (fstat(probe,&identity) || pread(probe,&header,sizeof(header),0)!=sizeof(header) ||
            header.initialized!=1 || header.owner_pid<=0 || kcf::detail::OwnerDead(header.owner_pid)) {
            last_error=-EAGAIN; close(probe); Disconnect(); return before_connected;
        }
        const bool same=identity_fd_>=0 && identity.st_dev==device_ && identity.st_ino==inode_;
        if (!same) {
            Disconnect();
            if (identity_fd_>=0) close(identity_fd_);
            identity_fd_=probe; device_=identity.st_dev; inode_=identity.st_ino;
            ++generation;
        } else close(probe);
        if (!connected) {
            last_error=subscriber.Create(name_,kcf::TopicStartPosition::OLDEST);
            if (!last_error && subscriber.GetDepth()!=depth) { subscriber.Close(); last_error=-EPROTO; }
            if (!last_error) {
                // Reject replacement during Subscriber::Create, not mix mappings.
                const int check=shm_open(shm_name_.c_str(),O_RDONLY|O_CLOEXEC,0600);
                struct stat current{};
                const bool stable=check>=0 && fstat(check,&current)==0 && current.st_dev==device_ && current.st_ino==inode_;
                if (check>=0) close(check);
                if (!stable) { subscriber.Close(); last_error=-EAGAIN; }
                else { connected=true; ++connections; }
            }
        }
        return before_generation!=generation || before_connected!=connected;
    }
    void Disconnect() { subscriber.Close(); connected=false; }
    kcf::Subscriber<T> subscriber;
    bool connected{};
    std::uint64_t generation{}, connections{};
    int last_error{};
private:
    std::string name_,shm_name_;
    // Retain the old fd while disconnected to prevent inode reuse ambiguity.
    int identity_fd_{-1};
    dev_t device_{};
    ino_t inode_{};
};
}
