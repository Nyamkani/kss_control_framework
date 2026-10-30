#pragma once
#include "kcf/network/control_frame.hpp"
#include "kcf/network/network_diagnostics.hpp"
#include <algorithm>
#include "kcf/network/peer_discovery.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace kcf::network::client_io {
using Clock=std::chrono::steady_clock;
struct Socket {int fd{-1};~Socket(){if(fd>=0)close(fd);}};
inline int Wait(int fd,short events,Clock::time_point deadline, const std::atomic<bool>* canceled = nullptr) {
    for(;;) {
        if(canceled && canceled->load())return -ECANCELED;
        const auto now=Clock::now();if(now>=deadline)return -ETIMEDOUT;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()+1;
        pollfd p{fd,events,0};const int result=poll(&p,1,static_cast<int>(canceled?std::min<std::int64_t>(remaining,20):remaining));
        if(result<0){if(errno==EINTR)continue;return -errno;}
        if(!result)continue;
        if(p.revents&POLLNVAL)return -EBADF;
        // Let send/recv/SO_ERROR report EOF or errors, including POLLHUP with data.
        return 0;
    }
}
inline int Transfer(int fd,std::uint8_t* bytes,std::size_t size,bool sending,Clock::time_point deadline, const std::atomic<bool>* canceled = nullptr) {
    std::size_t offset=0;
    while(offset<size) {
        if(canceled && canceled->load())return -ECANCELED;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        const auto count=sending?send(fd,bytes+offset,size-offset,MSG_NOSIGNAL):recv(fd,bytes+offset,size-offset,0);
        if(count>0){offset+=static_cast<std::size_t>(count);continue;}
        if(!count)return -ECONNRESET;
        if(errno==EINTR)continue;
        if(errno!=EAGAIN && errno!=EWOULDBLOCK)return -errno;
        const int code=Wait(fd,sending?POLLOUT:POLLIN,deadline,canceled);if(code)return code;
    }
    return 0;
}
inline int SendFrame(int fd,const ControlFrame& frame,Clock::time_point deadline, const std::atomic<bool>* canceled = nullptr) {
    std::vector<std::uint8_t> bytes;int code=EncodeControlFrame(frame,bytes);
    return code?code:Transfer(fd,bytes.data(),bytes.size(),true,deadline,canceled);
}
inline int ReceiveFrame(int fd,ControlMessage expected,ControlFrame& frame,Clock::time_point deadline, const std::atomic<bool>* canceled = nullptr) {
    std::vector<std::uint8_t> bytes(CONTROL_PREFIX_SIZE);
    int code=Transfer(fd,bytes.data(),bytes.size(),false,deadline,canceled);if(code)return code;
    std::size_t size=0;code=ControlFrameSize(bytes.data(),bytes.size(),size);if(code)return code;
    if(((unsigned(bytes[6])<<8)|bytes[7])!=static_cast<unsigned>(expected))return -EPROTO;
    const auto limit=expected==ControlMessage::HELLO?172:expected==ControlMessage::DIAGNOSTICS_RESPONSE?172+MAX_NETWORK_DIAGNOSTICS_PAYLOAD:MAX_CONTROL_FRAME;
    if(size>limit)return -EMSGSIZE;
    bytes.resize(size);code=Transfer(fd,bytes.data()+CONTROL_PREFIX_SIZE,size-CONTROL_PREFIX_SIZE,false,deadline,canceled);
    return code?code:DecodeControlFrame(bytes.data(),bytes.size(),frame);
}
// Shared connect/HELLO path; callers retain ownership and revalidate peer identity.
inline int ConnectHello(Socket& socket,const DiscoveryConfig& config,const PeerInfo& peer,
    const ControlFrame& hello,ControlFrame& greeting,Clock::time_point deadline,const std::atomic<bool>* canceled) {
    int code=0;
    socket.fd=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(socket.fd<0)return -errno;
    sockaddr_in local{};local.sin_family=AF_INET;
    if(inet_pton(AF_INET,config.interface_address.c_str(),&local.sin_addr)!=1)return -EINVAL;
    if(bind(socket.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local)))return -errno;
    sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(peer.port);
    if(inet_pton(AF_INET,peer.address.c_str(),&remote.sin_addr)!=1)return -EPROTO;
    if(Clock::now()>=deadline)return -ETIMEDOUT;
    if(connect(socket.fd,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))) {
        if(errno!=EINPROGRESS && errno!=EINTR && errno!=EAGAIN)return -errno;
        code=Wait(socket.fd,POLLOUT,deadline,canceled);if(code)return code;
        int error=0;socklen_t size=sizeof(error);
        if(getsockopt(socket.fd,SOL_SOCKET,SO_ERROR,&error,&size))return -errno;
        if(error)return -error;
    }
    code=SendFrame(socket.fd,hello,deadline,canceled);if(code)return code;
    code=ReceiveFrame(socket.fd,ControlMessage::HELLO,greeting,deadline,canceled);if(code)return code;
    return 0;
}
// Incremental, bounded stream reader. Partial prefix/body survives poll ticks;
// wire size validation and decoding stay in the existing Control Frame Codec.
struct FrameReader {
    std::vector<std::uint8_t> bytes;
    std::size_t target{CONTROL_PREFIX_SIZE};
    int Read(int fd,ControlFrame& frame) {
        for(;;) {
            if(bytes.size()==target) {
                if(target==CONTROL_PREFIX_SIZE) {
                    int code=ControlFrameSize(bytes.data(),bytes.size(),target);if(code)return code;
                    if(target>MAX_CONTROL_FRAME)return -EMSGSIZE;
                } else {
                    int code=DecodeControlFrame(bytes.data(),bytes.size(),frame);
                    bytes.clear();target=CONTROL_PREFIX_SIZE;return code?code:1;
                }
            }
            std::uint8_t buffer[4096];
            auto n=recv(fd,buffer,std::min(sizeof(buffer),target-bytes.size()),MSG_DONTWAIT);
            if(n>0){bytes.insert(bytes.end(),buffer,buffer+n);continue;}
            if(!n)return -ECONNRESET;
            if(errno==EINTR)continue;
            if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;
            return -errno;
        }
    }
};
}
