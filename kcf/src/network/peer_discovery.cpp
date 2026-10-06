#include "kcf/network/peer_discovery.hpp"
#include "kcf/network/validation.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <thread>
#include <system_error>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kcf::network
{
using Clock = std::chrono::steady_clock;
struct PeerDiscovery::Impl
{
    struct Entry
    {
        PeerInfo info;
        sockaddr_in address{};
        std::uint64_t sequence{0}, request_id{0};
        std::uint32_t size{0};
        std::uint16_t count{0};
        std::vector<std::uint8_t> staging;
        Clock::time_point next_request{}, next_response{};
        std::vector<std::pair<HostIdentity,std::uint64_t>> retired;
    };
    mutable std::mutex mutex;
    DiscoveryConfig config;
    DiscoveryStats stats;
    std::vector<Entry> peers;
    std::vector<std::uint8_t> metadata;
    std::uint16_t endpoint_count{0};
    std::uint64_t revision{1}, sequence{0}, session{0}, request_counter{0};
    int multicast_fd{-1}, unicast_fd{-1}, wake_fd{-1};
    sockaddr_in group{};
    std::atomic<bool> running{false};
    std::atomic<int> error{0};
    bool dirty{true};
    Clock::time_point next_announce{};
    std::thread worker;

    void CloseSockets() {
        for(int* fd:{&multicast_fd,&unicast_fd,&wake_fd}) {if(*fd>=0) close(*fd);*fd=-1;}
    }
    void Wake() {
        const std::uint64_t value=1;
        if(wake_fd>=0) {
            const auto written=write(wake_fd,&value,sizeof(value));
            (void)written; // Nonblocking wake is best effort; poll also has a bounded timeout.
        }
    }
    DiscoveryPacket Packet(DiscoveryMessage kind) {
        DiscoveryPacket p;p.kind=kind;p.host=config.host;p.boot_id=config.boot_id;p.session_id=session;return p;
    }
    bool Send(const DiscoveryPacket& p, const sockaddr_in& address) {
        std::vector<std::uint8_t> bytes;const int code=EncodeDiscoveryPacket(p,bytes);
        if(code) {error=code;return false;}
        const auto n=sendto(unicast_fd,bytes.data(),bytes.size(),MSG_DONTWAIT,
                            reinterpret_cast<const sockaddr*>(&address),sizeof(address));
        if(n<0) {if(errno!=EAGAIN && errno!=EWOULDBLOCK) error=-errno;return false;}
        return static_cast<std::size_t>(n)==bytes.size();
    }
    void BeginMetadata(Entry& e) {
        e.info.metadata_ready=false;e.info.endpoints.clear();e.staging.clear();
        if(request_counter==std::numeric_limits<std::uint64_t>::max()) {error=-EOVERFLOW;running=false;return;}
        e.request_id=++request_counter;e.next_request={};
    }
    void Tick(Clock::time_point now) {
        if(dirty || now>=next_announce) {
            if(sequence==std::numeric_limits<std::uint64_t>::max()) {error=-EOVERFLOW;running=false;return;}
            auto p=Packet(DiscoveryMessage::ANNOUNCE);p.sequence=++sequence;p.revision=revision;
            p.metadata_size=metadata.size();p.endpoint_count=endpoint_count;p.profile=config.profile;
            if(Send(p,group)) ++stats.announces_sent;
            dirty=false;next_announce=now+config.announce_interval;
        }
        for(auto& e:peers) {
            if(e.info.state==PeerState::ONLINE && now-e.info.last_seen>=config.peer_timeout) {
                e.info.state=PeerState::LOST;e.info.metadata_ready=false;e.info.endpoints.clear();e.staging.clear();
            }
            if(e.info.state==PeerState::ONLINE && !e.info.metadata_ready && now>=e.next_request) {
                auto p=Packet(DiscoveryMessage::METADATA_REQUEST);p.request_id=e.request_id;
                p.revision=e.info.metadata_revision;p.offset=e.staging.size();
                if(Send(p,e.address)) ++stats.metadata_requests_sent;
                e.next_request=now+config.metadata_retry;
            }
        }
    }
    static bool SameAddress(const sockaddr_in& a,const sockaddr_in& b) {
        return a.sin_addr.s_addr==b.sin_addr.s_addr && a.sin_port==b.sin_port;
    }
    void Announce(const DiscoveryPacket& p,const sockaddr_in& address,Clock::time_point now) {
        auto found=std::find_if(peers.begin(),peers.end(),[&](const Entry& e){return e.info.host==p.host;});
        if(found==peers.end()) {
            if(peers.size()==config.max_peers) {
                auto oldest=peers.end();
                for(auto it=peers.begin();it!=peers.end();++it)
                    if(it->info.state==PeerState::LOST && (oldest==peers.end() || it->info.last_seen<oldest->info.last_seen)) oldest=it;
                if(oldest==peers.end()) return;
                peers.erase(oldest);
            }
            peers.emplace_back();found=peers.end()-1;
        }
        auto& e=*found;
        const bool changed=e.info.session_id && (e.info.session_id!=p.session_id || e.info.boot_id!=p.boot_id);
        if(e.info.session_id && !changed && !(e.info.profile==p.profile)) return;
        if(changed) {
            // First live incarnation wins: cloned Host IDs must not flap the table.
            if(now-e.info.last_seen<config.peer_timeout) return;
            const auto key=std::make_pair(p.boot_id,p.session_id);
            if(std::find(e.retired.begin(),e.retired.end(),key)!=e.retired.end()) return;
            e.retired.emplace_back(e.info.boot_id,e.info.session_id);
            if(e.retired.size()>8) e.retired.erase(e.retired.begin());
            e.sequence=0;e.info.metadata_revision=0;e.next_response={};
        }
        if(p.sequence<=e.sequence || p.revision<e.info.metadata_revision) return;
        if(e.sequence && p.revision==e.info.metadata_revision && (e.size!=p.metadata_size || e.count!=p.endpoint_count)) return;
        const bool moved=e.sequence && !SameAddress(e.address,address);
        if(moved && now-e.info.last_seen<config.peer_timeout) return;
        const bool refresh=!e.info.session_id || changed || moved || e.info.state==PeerState::LOST || p.revision!=e.info.metadata_revision;
        e.info.profile=p.profile;
        e.info.compatibility=CheckPeerCompatibility(config.profile,p.profile,config.compatibility_policy);
        e.info.host=p.host;e.info.boot_id=p.boot_id;e.info.session_id=p.session_id;
        e.info.last_seen=now;e.info.state=PeerState::ONLINE;e.info.metadata_revision=p.revision;
        char ip[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip));e.info.address=ip;e.info.port=ntohs(address.sin_port);
        e.address=address;e.sequence=p.sequence;e.size=p.metadata_size;e.count=p.endpoint_count;
        if(refresh) BeginMetadata(e);
    }
    void Receive(const DiscoveryPacket& p,const sockaddr_in& address,bool multicast,Clock::time_point now) {
        if(p.host==config.host) return;
        if(multicast) {if(p.kind==DiscoveryMessage::ANNOUNCE) Announce(p,address,now);else ++stats.rejected_datagrams;return;}
        auto found=std::find_if(peers.begin(),peers.end(),[&](const Entry& e){return e.info.host==p.host;});
        if(found==peers.end()) return;
        auto& e=*found;
        if(e.info.state!=PeerState::ONLINE || e.info.boot_id!=p.boot_id || e.info.session_id!=p.session_id || !SameAddress(e.address,address)) return;
        if(p.kind==DiscoveryMessage::METADATA_REQUEST) {
            if(p.revision!=revision || p.offset>=metadata.size() || now<e.next_response) return;
            e.next_response=now+std::chrono::milliseconds(10); // bounded per-peer response rate
            auto response=Packet(DiscoveryMessage::METADATA_RESPONSE);response.request_id=p.request_id;
            response.revision=revision;response.offset=p.offset;response.metadata_size=metadata.size();
            const auto length=std::min<std::size_t>(DISCOVERY_CHUNK_SIZE,metadata.size()-p.offset);
            response.chunk.assign(metadata.begin()+p.offset,metadata.begin()+p.offset+length);
            if(Send(response,address)) ++stats.metadata_responses_sent;
        } else if(p.kind==DiscoveryMessage::METADATA_RESPONSE) {
            if(e.info.metadata_ready || p.request_id!=e.request_id || p.revision!=e.info.metadata_revision || p.metadata_size!=e.size || p.offset!=e.staging.size()) return;
            e.staging.insert(e.staging.end(),p.chunk.begin(),p.chunk.end());
            if(e.staging.size()==e.size) {
                std::vector<NetworkEndpointMetadata> decoded;
                const int result=DecodeDiscoveryMetadata(e.staging.data(),e.staging.size(),decoded);
                if(result || decoded.size()!=e.count || !ValidDiscoveryMetadata(decoded,p.host,p.boot_id)) {
                    ++stats.rejected_datagrams;BeginMetadata(e);e.next_request=now+config.metadata_retry;return;
                }
                e.info.endpoints=std::move(decoded);e.info.metadata_ready=true;e.staging.clear();
            } else e.next_request={};
        } else ++stats.rejected_datagrams;
    }
    void Drain(int fd,bool multicast) {
        for(unsigned i=0;i<32 && running;++i) {
            std::array<std::uint8_t,DISCOVERY_MAX_DATAGRAM> buffer{};sockaddr_in address{};socklen_t length=sizeof(address);
            const auto n=recvfrom(fd,buffer.data(),buffer.size(),MSG_DONTWAIT|MSG_TRUNC,reinterpret_cast<sockaddr*>(&address),&length);
            if(n<0) {if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) error=-errno;return;}
            DiscoveryPacket p;
            const bool bad=static_cast<std::size_t>(n)>buffer.size() || address.sin_family!=AF_INET || !address.sin_port ||
                address.sin_addr.s_addr==INADDR_ANY || IN_MULTICAST(ntohl(address.sin_addr.s_addr)) ||
                DecodeDiscoveryPacket(buffer.data(),n,p)!=0;
            std::lock_guard<std::mutex> lock(mutex);
            if(bad) ++stats.rejected_datagrams;
            else Receive(p,address,multicast,Clock::now());
        }
    }
    void Run() noexcept {
        try {
            while(running) {
                {std::lock_guard<std::mutex> lock(mutex);Tick(Clock::now());}
                pollfd fds[]{{multicast_fd,POLLIN,0},{unicast_fd,POLLIN,0},{wake_fd,POLLIN,0}};
                const int n=poll(fds,3,20);
                if(n<0) {if(errno==EINTR) continue;error=-errno;break;}
                if(fds[2].revents&POLLIN) {
                    std::uint64_t ignored;
                    const auto received=read(wake_fd,&ignored,sizeof(ignored));
                    (void)received;
                }
                if(!running) break;
                if((fds[0].revents|fds[1].revents)&(POLLNVAL|POLLHUP)) {error=-EBADF;break;}
                for(unsigned i=0;i<2;++i) if(fds[i].revents&POLLERR) {
                    int socket_error=0;socklen_t size=sizeof(socket_error);
                    if(getsockopt(fds[i].fd,SOL_SOCKET,SO_ERROR,&socket_error,&size)==0 && socket_error) error=-socket_error;
                }
                if(fds[0].revents&POLLIN) Drain(multicast_fd,true);
                if(fds[1].revents&POLLIN) Drain(unicast_fd,false);
            }
        } catch(const std::bad_alloc&) {error=-ENOMEM;}
          catch(...) {error=-EFAULT;}
        running=false;
    }
};
CompatibilityResult CheckRouteCompatibility(
    const DiscoveryConfig& local, const PeerInfo& peer,
    const NetworkEndpointMetadata& local_endpoint, const RemoteEndpointIdentity& remote_endpoint,
    TargetScope target)
{
    CompatibilityResult result;
    if(peer.state != PeerState::ONLINE) {result.reason=CompatibilityReason::PEER_LOST;return result;}
    result=CheckPeerCompatibility(local.profile,peer.profile,local.compatibility_policy);
    if(!result.Compatible()) return result;
    if(!peer.metadata_ready) {result.reason=CompatibilityReason::METADATA_UNAVAILABLE;return result;}
    if(local_endpoint.identity.runtime.host!=local.host || local_endpoint.identity.runtime.boot_id!=local.boot_id ||
       remote_endpoint.runtime.host!=peer.host || remote_endpoint.runtime.boot_id!=peer.boot_id) {
        result.reason=CompatibilityReason::HOST_MISMATCH;return result;
    }
    for(const auto& endpoint:peer.endpoints) if(endpoint.identity==remote_endpoint)
        return CheckEndpointCompatibility(local.profile,peer.profile,local_endpoint,endpoint,local.compatibility_policy,target);
    result.reason=CompatibilityReason::UNKNOWN_ENDPOINT;return result;
}
PeerDiscovery::PeerDiscovery():impl_(std::make_unique<Impl>()) {}
PeerDiscovery::~PeerDiscovery() {Stop();}
int PeerDiscovery::Start(const DiscoveryConfig& config,const std::vector<NetworkEndpointMetadata>& endpoints)
{
    auto& s=*impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if(s.worker.joinable()) return -EBUSY;
    in_addr interface{},group{};
    if(!CheckPeerCompatibility(config.profile,config.profile,config.compatibility_policy).Compatible() ||
       !(config.profile.capabilities & CapabilityBit(Capability::DISCOVERY)) ||
       !ValidDiscoveryMetadata(endpoints,config.host,config.boot_id) || !config.port ||
       inet_pton(AF_INET,config.interface_address.c_str(),&interface)!=1 || inet_pton(AF_INET,config.multicast_group.c_str(),&group)!=1 ||
       (ntohl(group.s_addr)&0xff000000u)!=0xef000000u || IN_MULTICAST(ntohl(interface.s_addr)) || interface.s_addr==INADDR_BROADCAST ||
       config.announce_interval.count()<20 || config.announce_interval.count()>60000 ||
       config.peer_timeout.count()<config.announce_interval.count()*3 || config.peer_timeout.count()>300000 ||
       config.metadata_retry.count()<20 || config.metadata_retry>config.peer_timeout || !config.max_peers || config.max_peers>DISCOVERY_MAX_PEERS) return -EINVAL;
    try {
        std::vector<std::uint8_t> bytes;const int encoded=EncodeDiscoveryMetadata(endpoints,bytes);if(encoded) return encoded;
        std::uint64_t session=0;ssize_t got;
        do {got=getrandom(&session,sizeof(session),GRND_NONBLOCK);} while(got<0 && errno==EINTR);
        if(got!=sizeof(session) || !session) return got<0?-errno:-EIO;
        s.config=config;s.metadata=std::move(bytes);s.endpoint_count=endpoints.size();s.session=session;
        s.revision=1;s.sequence=0;s.request_counter=0;s.peers.clear();s.stats={};s.error=0;s.dirty=true;
        const auto fail=[&] {const int e=errno;s.CloseSockets();return -e;};
        s.multicast_fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(s.multicast_fd<0) return fail();
        int one=1,zero=0;
        if(setsockopt(s.multicast_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))) return fail();
        sockaddr_in bind_address{};bind_address.sin_family=AF_INET;bind_address.sin_port=htons(config.port);bind_address.sin_addr.s_addr=INADDR_ANY;
        if(bind(s.multicast_fd,reinterpret_cast<sockaddr*>(&bind_address),sizeof(bind_address))) return fail();
        ip_mreq membership{};membership.imr_multiaddr=group;membership.imr_interface=interface;
        if(setsockopt(s.multicast_fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&membership,sizeof(membership))) return fail();
        if(setsockopt(s.multicast_fd,IPPROTO_IP,IP_MULTICAST_ALL,&zero,sizeof(zero))) return fail();
        s.unicast_fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(s.unicast_fd<0) return fail();
        bind_address.sin_port=htons(config.metadata_port);bind_address.sin_addr=interface;
        if(bind(s.unicast_fd,reinterpret_cast<sockaddr*>(&bind_address),sizeof(bind_address))) return fail();
        unsigned char ttl=1,loop=1;
        if(setsockopt(s.unicast_fd,IPPROTO_IP,IP_MULTICAST_IF,&interface,sizeof(interface)) ||
           setsockopt(s.unicast_fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl)) ||
           setsockopt(s.unicast_fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop))) return fail();
        s.group={};s.group.sin_family=AF_INET;s.group.sin_port=htons(config.port);s.group.sin_addr=group;
        s.wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(s.wake_fd<0) return fail();
        s.running=true;
        try {s.worker=std::thread([&s]{s.Run();});}
        catch(...) {s.running=false;throw;}
        return 0;
    } catch(const std::bad_alloc&) {s.CloseSockets();return -ENOMEM;}
      catch(const std::system_error& e) {s.CloseSockets();return -e.code().value();}
}
int PeerDiscovery::UpdateMetadata(const std::vector<NetworkEndpointMetadata>& endpoints)
{
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.running) return -ENOTCONN;
    if(!ValidDiscoveryMetadata(endpoints,s.config.host,s.config.boot_id)) return -EINVAL;
    std::vector<std::uint8_t> bytes;const int result=EncodeDiscoveryMetadata(endpoints,bytes);if(result) return result;
    if(bytes==s.metadata) return 0;
    if(s.revision==std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
    s.metadata=std::move(bytes);s.endpoint_count=endpoints.size();++s.revision;s.dirty=true;s.Wake();return 0;
}
void PeerDiscovery::Stop()
{
    auto& s=*impl_;
    {std::lock_guard<std::mutex> lock(s.mutex);s.running=false;s.Wake();}
    if(s.worker.joinable()) s.worker.join();
    std::lock_guard<std::mutex> lock(s.mutex);s.CloseSockets();s.peers.clear();
}
std::vector<PeerInfo> PeerDiscovery::GetPeers() const
{
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);std::vector<PeerInfo> result;
    result.reserve(s.peers.size());for(const auto& e:s.peers) result.push_back(e.info);return result;
}
DiscoveryStats PeerDiscovery::GetStats() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->stats;}
std::uint64_t PeerDiscovery::GetSessionId() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->session;}
int PeerDiscovery::GetLastError() const {return impl_->error.load();}
} // namespace kcf::network
