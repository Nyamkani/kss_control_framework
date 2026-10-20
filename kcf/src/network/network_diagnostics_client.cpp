#include "control_client_io.hpp"
#include "kcf/network/network_diagnostics_client.hpp"
#include "kcf/network/control_frame.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <limits>
#include <new>

namespace kcf::network {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> next_request{0};
int NextRequest(std::uint64_t& id) {
    auto previous=next_request.load();
    do {if(previous==std::numeric_limits<std::uint64_t>::max())return -EOVERFLOW;}
    while(!next_request.compare_exchange_weak(previous,previous+1));
    id=previous+1;return 0;
}
using namespace client_io;
int Peer(PeerDiscovery& discovery,const DiscoveryConfig& config,const HostIdentity& host,PeerInfo& out) {
    if(discovery.GetLastError())return discovery.GetLastError();
    for(const auto& peer:discovery.GetPeers())if(peer.host==host) {
        if(peer.state!=PeerState::ONLINE)return -ENOTCONN;
        if(!CheckPeerCompatibility(config.profile,peer.profile,config.compatibility_policy).Compatible())return -EPROTONOSUPPORT;
        if(!(peer.profile.capabilities&CapabilityBit(Capability::NETWORK_DIAGNOSTICS)))return -ENOTSUP;
        in_addr ip{};
        if(!Valid(peer.host)||!Valid(peer.boot_id)||!peer.session_id||!peer.port||
           inet_pton(AF_INET,peer.address.c_str(),&ip)!=1||!ip.s_addr||
           ip.s_addr==INADDR_BROADCAST||IN_MULTICAST(ntohl(ip.s_addr)))return -EPROTO;
        out=peer;return 0;
    }
    return -ENOENT;
}
bool Identity(const ControlFrame& frame,const PeerInfo& peer) {
    return frame.source.host==peer.host && frame.source.boot_id==peer.boot_id && frame.session_id==peer.session_id;
}
}
NetworkDiagnosticsClient::NetworkDiagnosticsClient(PeerDiscovery& discovery,const DiscoveryConfig& config)
    :discovery_(discovery),config_(config) {}
int NetworkDiagnosticsClient::Query(const HostIdentity& gateway,NetworkDiagnosticsSnapshot& snapshot,std::chrono::milliseconds timeout) {
    if(timeout.count()<1||timeout.count()>60000||!Valid(gateway)||gateway==config_.host||
       !Valid(config_.host)||!Valid(config_.boot_id)||
       !IsObserverProfile(config_.profile))return -EINVAL;
    const auto deadline=Clock::now()+timeout;
    std::unique_lock<std::timed_mutex> lock(mutex_,std::defer_lock);
    if(!lock.try_lock_until(deadline))return -ETIMEDOUT;
    try {
        const auto session=discovery_.GetSessionId();if(!session)return -ENOTCONN;
        PeerInfo peer;int code=Peer(discovery_,config_,gateway,peer);if(code)return code;
        const auto fresh=[&] {
            if(discovery_.GetSessionId()!=session)return -ESTALE;
            PeerInfo current;int error=Peer(discovery_,config_,gateway,current);if(error)return error;
            return current.boot_id==peer.boot_id && current.session_id==peer.session_id &&
                   current.address==peer.address && current.port==peer.port?0:-ESTALE;
        };
        ControlFrame hello;hello.source={config_.host,config_.boot_id,getpid(),0};hello.session_id=session;
        if(!kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks))return -ESRCH;
        Socket socket;socket.fd=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(socket.fd<0)return -errno;
        sockaddr_in local{};local.sin_family=AF_INET;
        if(inet_pton(AF_INET,config_.interface_address.c_str(),&local.sin_addr)!=1)return -EINVAL;
        if(bind(socket.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local)))return -errno;
        sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(peer.port);
        if(inet_pton(AF_INET,peer.address.c_str(),&remote.sin_addr)!=1)return -EPROTO;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        if(connect(socket.fd,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))) {
            if(errno!=EINPROGRESS && errno!=EINTR && errno!=EAGAIN)return -errno;
            code=Wait(socket.fd,POLLOUT,deadline);if(code)return code;
            int error=0;socklen_t size=sizeof(error);
            if(getsockopt(socket.fd,SOL_SOCKET,SO_ERROR,&error,&size))return -errno;
            if(error)return -error;
        }
        code=SendFrame(socket.fd,hello,deadline);if(code)return code;
        ControlFrame greeting;code=ReceiveFrame(socket.fd,ControlMessage::HELLO,greeting,deadline);if(code)return code;
        if(!Identity(greeting,peer))return -ESTALE;
        code=fresh();if(code)return code;
        auto request=hello;request.operation=ControlMessage::DIAGNOSTICS_REQUEST;request.target=TargetScope::HOST;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<1)return -ETIMEDOUT;
        request.timeout_ms=static_cast<std::uint32_t>(remaining);
        code=NextRequest(request.request_id);if(code)return code;
        code=SendFrame(socket.fd,request,deadline);if(code)return code;
        ControlFrame response;code=ReceiveFrame(socket.fd,ControlMessage::DIAGNOSTICS_RESPONSE,response,deadline);if(code)return code;
        if(response.request_id!=request.request_id)return -EPROTO;
        if(!Identity(response,peer)||response.source!=greeting.source)return -ESTALE;
        code=fresh();if(code)return code;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        if(response.status)return response.status;
        NetworkDiagnosticsSnapshot decoded;
        code=DecodeNetworkDiagnosticsSnapshot(response.payload.data(),response.payload.size(),decoded);if(code)return code;
        if(decoded.gateway_host!=peer.host||decoded.gateway_boot_id!=peer.boot_id||decoded.gateway_session!=peer.session_id)return -ESTALE;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        snapshot=std::move(decoded);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
}
