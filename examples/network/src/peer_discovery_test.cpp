#include "kcf/network/peer_discovery.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <cassert>
#include <atomic>
#include <sys/syscall.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <random>
#include <csignal>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>
// Fault injection is confined to this test executable and selected socket FDs.
static std::atomic<int> failed_send_fd{-1}, failed_receive_fd{-1};
static std::atomic<int> injected_send_error{ENETUNREACH};
extern "C" ssize_t sendto(int fd,const void* data,size_t size,int flags,const sockaddr* address,socklen_t length) {
    if(fd==failed_send_fd.load()) {errno=injected_send_error.load();return -1;}
    return syscall(SYS_sendto,fd,data,size,flags,address,length);
}
extern "C" ssize_t recvfrom(int fd,void* data,size_t size,int flags,sockaddr* address,socklen_t* length) {
    if(fd==failed_receive_fd.load()) {errno=EHOSTUNREACH;return -1;}
    return syscall(SYS_recvfrom,fd,data,size,flags,address,length);
}
using namespace std::chrono_literals;
namespace n=kcf::network;
n::HostIdentity Id(unsigned char x) {n::HostIdentity id;id.bytes[0]=x;return id;}
n::DiscoveryConfig Config(unsigned char host,std::uint16_t port) {
    n::DiscoveryConfig c;c.host=Id(host);c.boot_id=Id(42);c.interface_address="127.0.0.1";c.port=port;
    c.announce_interval=60ms;c.peer_timeout=400ms;c.metadata_retry=40ms;return c;
}
std::vector<n::NetworkEndpointMetadata> Metadata(unsigned char host,unsigned count=4) {
    std::vector<n::NetworkEndpointMetadata> result;
    for(unsigned i=0;i<count;++i) {
        n::NetworkEndpointMetadata m;m.identity={{Id(host),Id(42),123,456},i+1};
        m.application={Id(host),Id(42),99,111};m.host_name="host";m.application_name="app";m.element_name="element";
        m.name="/endpoint/"+std::to_string(i);m.kind=static_cast<n::EndpointKind>(i%4+1);m.scope=n::NetworkScope::REMOTE;
        n::TypeIdentity type{10,20,1,4};
        if(i%4<2) m.value_type=type;else {m.request_type=type;m.response_type=type;}
        if(i%4==3) m.feedback_type=type;
        m.groups={"robots"};result.push_back(m);
    }
    return result;
}
template<class F> void Until(F predicate) {
    const auto deadline=std::chrono::steady_clock::now()+8s;
    while(!predicate()) {assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(10ms);}
}
bool Has(n::PeerDiscovery& d,unsigned char host,n::PeerState state,bool ready=false,std::size_t count=0) {
    for(const auto& p:d.GetPeers()) if(p.host==Id(host))
        return p.state==state && (!ready || (p.metadata_ready && p.endpoints.size()==count));
    return false;
}
std::size_t Fds() {return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});}
std::uint16_t FreePort() {
    int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t length=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&length)==0);
    close(fd);return ntohs(a.sin_port);
}
struct Injector {
    int fd; sockaddr_in group{};
    explicit Injector(std::uint16_t port) {
        fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);in_addr loop{};assert(inet_pton(AF_INET,"127.0.0.1",&loop)==1);
        assert(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&loop,sizeof(loop))==0);
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr=loop;assert(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
        group.sin_family=AF_INET;group.sin_port=htons(port);assert(inet_pton(AF_INET,"239.255.75.67",&group.sin_addr)==1);
    }
    ~Injector(){close(fd);}
    void Send(const std::vector<std::uint8_t>& bytes) {
        assert(sendto(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&group),sizeof(group))==static_cast<ssize_t>(bytes.size()));
    }
};
n::DiscoveryPacket Announce(unsigned char id=90) {
    n::DiscoveryPacket p;p.host=Id(id);p.boot_id=Id(42);p.session_id=7;p.sequence=1;p.revision=1;p.metadata_size=2;return p;
}
// Controlled peer: reply one requested chunk at a time, with real UDP framing.
struct MetadataPeer : Injector {
    n::DiscoveryPacket announce=Announce();
    std::vector<std::uint8_t> metadata;
    explicit MetadataPeer(std::uint16_t port):Injector(port) {}
    void Heartbeat() {
        ++announce.sequence;
        std::vector<std::uint8_t> bytes;assert(n::EncodeDiscoveryPacket(announce,bytes)==0);Send(bytes);
    }
    void Revision(std::uint64_t revision,const std::vector<n::NetworkEndpointMetadata>& endpoints) {
        assert(n::EncodeDiscoveryMetadata(endpoints,metadata)==0);
        announce.revision=revision;announce.metadata_size=metadata.size();announce.endpoint_count=endpoints.size();Heartbeat();
    }
    struct Request {n::DiscoveryPacket packet;sockaddr_in address;};
    Request Next(std::size_t offset) {
        const auto deadline=std::chrono::steady_clock::now()+2s;
        auto heartbeat=std::chrono::steady_clock::time_point{};
        for(;;) {
            const auto now=std::chrono::steady_clock::now();assert(now<deadline);
            if(now>=heartbeat){Heartbeat();heartbeat=now+60ms;}
            pollfd ready{fd,POLLIN,0};assert(poll(&ready,1,20)>=0);
            if(!(ready.revents&POLLIN))continue;
            std::array<std::uint8_t,n::DISCOVERY_MAX_DATAGRAM> bytes{};Request request{};socklen_t size=sizeof(request.address);
            const auto count=recvfrom(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&request.address),&size);assert(count>0);
            assert(n::DecodeDiscoveryPacket(bytes.data(),count,request.packet)==0);
            if(request.packet.kind==n::DiscoveryMessage::METADATA_REQUEST &&
               request.packet.revision==announce.revision && request.packet.offset==offset)return request;
        }
    }
    n::DiscoveryPacket Response(const Request& request) const {
        n::DiscoveryPacket response;response.kind=n::DiscoveryMessage::METADATA_RESPONSE;
        response.host=announce.host;response.boot_id=announce.boot_id;response.session_id=announce.session_id;
        response.request_id=request.packet.request_id;response.revision=request.packet.revision;
        response.metadata_size=metadata.size();response.offset=request.packet.offset;
        const auto end=std::min(metadata.size(),response.offset+n::DISCOVERY_CHUNK_SIZE);
        response.chunk.assign(metadata.begin()+response.offset,metadata.begin()+end);return response;
    }
    void Reply(const Request& request,const n::DiscoveryPacket& response) {
        std::vector<std::uint8_t> bytes;assert(n::EncodeDiscoveryPacket(response,bytes)==0);
        assert(sendto(fd,bytes.data(),bytes.size(),0,reinterpret_cast<const sockaddr*>(&request.address),sizeof(request.address))==static_cast<ssize_t>(bytes.size()));
    }
    void Complete(std::size_t offset=0) {
        while(offset<metadata.size()) {
            auto request=Next(offset);auto response=Response(request);Reply(request,response);offset+=response.chunk.size();
        }
    }
};
n::PeerInfo OnlyPeer(n::PeerDiscovery& observer) {
    auto peers=observer.GetPeers();assert(peers.size()==1);return peers[0];
}
void NoSnapshot(const n::PeerInfo& peer) {
    assert(!peer.metadata_ready && peer.endpoints.empty());
    assert(!peer.last_complete_snapshot.valid && !peer.last_complete_snapshot.stale);
    assert(!peer.last_complete_snapshot.revision && peer.last_complete_snapshot.endpoints.empty());
}
void SnapshotRevisions() {
    const auto baseline=Fds();
    {
        const auto port=FreePort();auto config=Config(1,port);n::PeerDiscovery observer;
        assert(observer.Start(config)==0);MetadataPeer remote(port);
        const auto initial=Metadata(90,32);remote.Revision(1,initial);
        auto request=remote.Next(0);NoSnapshot(OnlyPeer(observer));
        remote.Reply(request,remote.Response(request));
        remote.Complete(n::DISCOVERY_CHUNK_SIZE);
        Until([&]{return OnlyPeer(observer).metadata_ready;});
        auto original=OnlyPeer(observer);
        assert(original.endpoints.size()==32 && original.last_complete_snapshot.valid && !original.last_complete_snapshot.stale);
        assert(original.last_complete_snapshot.revision==1 && original.last_complete_snapshot.endpoints.size()==32);
        // Returned vectors are independent copies, including nested metadata strings.
        auto copy=original;copy.endpoints.clear();copy.last_complete_snapshot.endpoints[0].name="changed-copy";
        assert(OnlyPeer(observer).last_complete_snapshot.endpoints[0].name==initial[0].name);
        auto stale=[&](std::uint64_t revision,std::uint64_t completed,std::size_t count) {
            const auto peer=OnlyPeer(observer);assert(peer.state==n::PeerState::ONLINE);
            assert(peer.metadata_revision==revision && !peer.metadata_ready && peer.endpoints.empty());
            assert(peer.last_complete_snapshot.valid && peer.last_complete_snapshot.stale);
            assert(peer.last_complete_snapshot.revision==completed && peer.last_complete_snapshot.endpoints.size()==count);
            assert(n::CheckRouteCompatibility(config,peer,Metadata(1)[0],initial[0].identity).reason==n::CompatibilityReason::METADATA_UNAVAILABLE);
        };
        remote.Revision(2,Metadata(90,16));request=remote.Next(0);stale(2,1,32);
        remote.Reply(request,remote.Response(request));
        auto missing=remote.Next(n::DISCOVERY_CHUNK_SIZE);
        auto retry=remote.Next(n::DISCOVERY_CHUNK_SIZE); // Deliberately lose a response.
        assert(retry.packet.request_id==missing.packet.request_id);stale(2,1,32);
        auto wrong=remote.Response(retry);++wrong.request_id;remote.Reply(retry,wrong);
        wrong=remote.Response(retry);++wrong.revision;remote.Reply(retry,wrong);
        wrong=remote.Response(retry);wrong.offset=0;remote.Reply(retry,wrong); // Duplicate/out-of-order.
        remote.Next(n::DISCOVERY_CHUNK_SIZE);stale(2,1,32);
        remote.Complete(n::DISCOVERY_CHUNK_SIZE);
        Until([&]{return OnlyPeer(observer).metadata_ready;});
        auto replacement=OnlyPeer(observer);
        assert(replacement.endpoints.size()==16 && replacement.last_complete_snapshot.endpoints.size()==16);
        assert(replacement.last_complete_snapshot.revision==2 && !replacement.last_complete_snapshot.stale);
        // A complete but invalid revision must restart collection, not replace display data.
        remote.Revision(3,Metadata(90,16));remote.metadata[0]=255;
        request=remote.Next(0);remote.Reply(request,remote.Response(request));
        const auto rejected=observer.GetStats().rejected_datagrams;
        remote.Complete(n::DISCOVERY_CHUNK_SIZE);
        Until([&]{return observer.GetStats().rejected_datagrams>rejected;});
        auto restarted=remote.Next(0);assert(restarted.packet.request_id!=request.packet.request_id);stale(3,2,16);
        // Consecutive revisions discard staging, even if an old response arrives late.
        remote.Revision(4,Metadata(90,16));request=remote.Next(0);
        const auto late=remote.Response(request);remote.Reply(request,late);
        remote.Next(n::DISCOVERY_CHUNK_SIZE);stale(4,2,16);
        remote.Revision(5,Metadata(90,1));auto current=remote.Next(0);
        remote.Reply(request,late);stale(5,2,16);
        remote.Reply(current,remote.Response(current));
        Until([&]{return OnlyPeer(observer).metadata_ready;});
        auto fresh=OnlyPeer(observer);
        assert(fresh.endpoints.size()==1 && fresh.last_complete_snapshot.endpoints.size()==1);
        assert(fresh.last_complete_snapshot.revision==5 && !fresh.last_complete_snapshot.stale);
        assert(original.endpoints.size()==32 && original.last_complete_snapshot.revision==1);
        // A verified empty revision is valid and distinct from never having metadata.
        remote.Revision(6,{});remote.Complete();Until([&]{return OnlyPeer(observer).metadata_ready;});
        fresh=OnlyPeer(observer);assert(fresh.endpoints.empty() && fresh.last_complete_snapshot.endpoints.empty());
        assert(fresh.last_complete_snapshot.valid && !fresh.last_complete_snapshot.stale && fresh.last_complete_snapshot.revision==6);
        remote.Revision(7,initial);remote.Next(0);stale(7,6,0);
        Until([&]{return OnlyPeer(observer).state==n::PeerState::LOST;});NoSnapshot(OnlyPeer(observer));
        // Same generation after loss cannot resurrect the old display snapshot.
        remote.Revision(8,Metadata(90,1));remote.Next(0);NoSnapshot(OnlyPeer(observer));
        remote.Complete();Until([&]{return OnlyPeer(observer).metadata_ready;});
        // First live incarnation wins; accepted new Session/Boot must start without a snapshot.
        ++remote.announce.session_id;remote.Heartbeat();
        std::this_thread::sleep_for(40ms);assert(OnlyPeer(observer).session_id==7);
        Until([&]{return OnlyPeer(observer).state==n::PeerState::LOST;});
        remote.Revision(1,Metadata(90,1));remote.Next(0);NoSnapshot(OnlyPeer(observer));
        assert(OnlyPeer(observer).session_id==remote.announce.session_id);
        remote.Complete();Until([&]{return OnlyPeer(observer).metadata_ready;});
        Until([&]{return OnlyPeer(observer).state==n::PeerState::LOST;});
        remote.announce.boot_id=Id(43);auto rebooted=Metadata(90,1);
        for(auto& e:rebooted){e.identity.runtime.boot_id=Id(43);e.application.boot_id=Id(43);}
        remote.Revision(1,rebooted);remote.Next(0);NoSnapshot(OnlyPeer(observer));
        remote.Complete();Until([&]{return OnlyPeer(observer).metadata_ready;});
        assert(OnlyPeer(observer).boot_id==Id(43));
        // Moving the same Host/Boot/Session to a different metadata socket also invalidates.
        MetadataPeer moved(port);moved.announce=remote.announce;moved.Revision(1,rebooted);
        std::this_thread::sleep_for(40ms);assert(OnlyPeer(observer).port!=0);
        const auto old_port=OnlyPeer(observer).port;
        Until([&]{return OnlyPeer(observer).state==n::PeerState::LOST;});
        moved.Heartbeat();moved.Next(0);NoSnapshot(OnlyPeer(observer));assert(OnlyPeer(observer).port!=old_port);
        moved.Complete();Until([&]{return OnlyPeer(observer).metadata_ready;});
        // Fault only this observer's multicast socket. No descriptors are opened
        // between closing it and Stop(), so its eventual EBADF close cannot hit a reused fd.
        int failed=-1;
        for(const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) {
            const int fd=std::stoi(entry.path().filename().string());sockaddr_in address{};socklen_t size=sizeof(address);
            if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)==0 && address.sin_family==AF_INET && ntohs(address.sin_port)==port)failed=fd;
        }
        assert(failed>=0 && close(failed)==0);
        Until([&]{return observer.GetLastError()!=0;});NoSnapshot(OnlyPeer(observer));
        assert(OnlyPeer(observer).state==n::PeerState::LOST);
        observer.Stop();assert(observer.GetPeers().empty());
    }
    assert(Fds()==baseline);
    std::cout<<"display snapshots: chunks/retry/invalid/consecutive revisions/removal/empty/LOST/session/boot/move/failure/copy/route rejection PASS\n";
}
void Codec() {
    std::vector<std::uint8_t> bytes;auto original=Announce();assert(n::EncodeDiscoveryPacket(original,bytes)==0);
    assert(bytes.size()==100 && bytes[0]=='K' && bytes[1]=='C' && bytes[2]=='F' && bytes[3]=='D');
    n::DiscoveryPacket decoded;assert(n::DecodeDiscoveryPacket(bytes.data(),bytes.size(),decoded)==0 && decoded.host==original.host);
    for(std::size_t i=0;i<bytes.size();++i) {
        decoded.host=Id(99);assert(n::DecodeDiscoveryPacket(bytes.data(),i,decoded)!=0 && decoded.host==Id(99));
    }
    assert(bytes[5]==2 && bytes[11]==100 && bytes[81]==5 && bytes[83]==6 && bytes[87]==1 && bytes[99]==1);
    assert(decoded.profile==original.profile);
    auto custom=original;custom.profile.framework={9,8,7};custom.profile.network={2,3};
    custom.profile.capabilities=n::KNOWN_CAPABILITIES | (1ull<<63);
    std::vector<std::uint8_t> versioned;assert(n::EncodeDiscoveryPacket(custom,versioned)==0);
    assert(n::DecodeDiscoveryPacket(versioned.data(),versioned.size(),decoded)==0 && decoded.profile==custom.profile);
    for(auto offset:{86,90,92}) {
        auto broken=bytes;
        if(offset==86) broken[87]=0; // invalid Network major
        if(offset==90) broken[91]=1; // reserved
        if(offset==92) broken[99]=0; // missing DISCOVERY
        assert(n::DecodeDiscoveryPacket(broken.data(),broken.size(),decoded)!=0);
    }
    auto legacy=bytes;legacy.resize(80);legacy[5]=1;legacy[11]=80;
    assert(n::DecodeDiscoveryPacket(legacy.data(),legacy.size(),decoded)==-EPROTONOSUPPORT);
    auto short_v2=legacy;short_v2[5]=2;
    assert(n::DecodeDiscoveryPacket(short_v2.data(),short_v2.size(),decoded)!=0);
    auto bad=bytes;bad[5]=1;assert(n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded)==-EPROTONOSUPPORT);
    bad=bytes;bad[5]=99;assert(n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded)==-EPROTONOSUPPORT);
    for(auto offset:{0,7,11,15}) {bad=bytes;bad[offset]^=0xff;assert(n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded)!=0);}
    bad=bytes;std::fill(bad.begin()+16,bad.begin()+32,0);assert(n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded)!=0);
    bad=bytes;bad.push_back(0);assert(n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded)!=0);
    assert(n::DecodeDiscoveryPacket(nullptr,80,decoded)==-EINVAL);
    auto endpoints=Metadata(1,32);assert(n::EncodeDiscoveryMetadata(endpoints,bytes)==0 && bytes.size()>n::DISCOVERY_CHUNK_SIZE);
    std::vector<n::NetworkEndpointMetadata> roundtrip;assert(n::DecodeDiscoveryMetadata(bytes.data(),bytes.size(),roundtrip)==0);
    assert(roundtrip.size()==32 && n::ValidDiscoveryMetadata(roundtrip,Id(1),Id(42)));
    for(std::size_t i=0;i<bytes.size();++i) assert(n::DecodeDiscoveryMetadata(bytes.data(),i,roundtrip)!=0 && roundtrip.size()==32);
    bad=bytes;bad.push_back(0);assert(n::DecodeDiscoveryMetadata(bad.data(),bad.size(),roundtrip)!=0);
    bad=bytes;bad[1]=255;assert(n::DecodeDiscoveryMetadata(bad.data(),bad.size(),roundtrip)!=0);
    endpoints[1].identity=endpoints[0].identity;assert(!n::ValidDiscoveryMetadata(endpoints,Id(1),Id(42)));
    // Bounded random external bytes exercise decode without requiring allocation
    // according to attacker-supplied sizes. Sanitizer run uses the same corpus.
    std::mt19937 random(123);
    for(unsigned i=0;i<5000;++i) {
        bad.resize(random()%1400);for(auto& b:bad)b=random();
        (void)n::DecodeDiscoveryPacket(bad.data(),bad.size(),decoded);
        (void)n::DecodeDiscoveryMetadata(bad.data(),bad.size(),roundtrip);
    }
    for(unsigned i=0;i<200;++i) {
        bad=bytes;for(unsigned j=0;j<4;++j)bad[random()%bad.size()]=random();
        (void)n::DecodeDiscoveryMetadata(bad.data(),bad.size(),roundtrip);
    }
    std::cout<<"wire codec/limits/truncation/random input PASS\n";
}
void Integration(const char* executable) {
    const auto baseline=Fds();const auto port=FreePort();
    {
        n::PeerDiscovery a,b,c;auto ca=Config(1,port),cb=Config(2,port),cc=Config(3,port);
        assert(a.Start(ca,Metadata(1))==0);assert(a.Start(ca)==-EBUSY);
        std::this_thread::sleep_for(150ms);assert(a.GetPeers().empty());
        assert(b.Start(cb,Metadata(2))==0);
        Until([&]{return Has(a,2,n::PeerState::ONLINE,true,4) && Has(b,1,n::PeerState::ONLINE,true,4);});
        assert(c.Start(cc,Metadata(3))==0);
        Until([&]{return a.GetPeers().size()==2 && b.GetPeers().size()==2 && c.GetPeers().size()==2 &&
            Has(a,3,n::PeerState::ONLINE,true,4) && Has(b,3,n::PeerState::ONLINE,true,4) &&
            Has(c,1,n::PeerState::ONLINE,true,4) && Has(c,2,n::PeerState::ONLINE,true,4);});
        for(const auto& p:a.GetPeers()) {
            assert(p.host!=ca.host && p.endpoints[0].identity.runtime.pid==123 && p.endpoints[0].identity.runtime.process_start_ticks==456);
            assert(p.endpoints[0].identity.runtime.host==p.host && p.port && p.address=="127.0.0.1");
        }
        kcf::SharedChannel<std::uint64_t> local,reader;
        const std::string local_name="/kcf_discovery_local_"+std::to_string(getpid());
        assert(local.Create(local_name)==0 && reader.Open(local_name)==0);
        for(std::uint64_t i=1;i<=200;++i) {
            std::uint64_t value=0,seq=0;assert(local.Publish(i)==0 && reader.ReadLatestSnapshot(value,seq)==0 && value==i && seq==i);
            std::this_thread::sleep_for(1ms);
        }
        const auto requests=a.GetStats().metadata_requests_sent;
        std::this_thread::sleep_for(200ms);assert(a.GetStats().metadata_requests_sent==requests); // no repeated full metadata
        auto larger=Metadata(2,32);assert(b.UpdateMetadata(larger)==0);
        Until([&]{return Has(a,2,n::PeerState::ONLINE,true,32) && Has(c,2,n::PeerState::ONLINE,true,32);});
        auto bad=larger;bad[0].identity.runtime.host=Id(9);assert(b.UpdateMetadata(bad)==-EINVAL);
        assert(b.UpdateMetadata({})==0);Until([&]{return Has(a,2,n::PeerState::ONLINE,true,0);});
        // Malformed network traffic and duplicate announce do not create/refresh peers.
        Injector inject(port);auto p=Announce();std::vector<std::uint8_t> wire;assert(n::EncodeDiscoveryPacket(p,wire)==0);
        auto malformed=wire;malformed[5]=1;const auto rejects=a.GetStats().rejected_datagrams;
        inject.Send(malformed);malformed=wire;malformed[0]=0;inject.Send(malformed);
        malformed.assign(1500,0);inject.Send(malformed);
        Until([&]{return a.GetStats().rejected_datagrams>=rejects+3;});assert(a.GetPeers().size()==2);
        inject.Send(wire);Until([&]{return Has(a,90,n::PeerState::ONLINE);});
        auto before=a.GetPeers();std::chrono::steady_clock::time_point seen;
        for(const auto& peer:before) if(peer.host==Id(90)) seen=peer.last_seen;
        auto changed=p;changed.sequence++;changed.profile.framework.patch++;
        std::vector<std::uint8_t> changed_wire;assert(n::EncodeDiscoveryPacket(changed,changed_wire)==0);
        inject.Send(changed_wire); // A profile cannot change inside one session.
        for(int i=0;i<8;++i) {inject.Send(wire);std::this_thread::sleep_for(60ms);}
        Until([&]{return Has(a,90,n::PeerState::LOST);});
        for(const auto& peer:a.GetPeers()) if(peer.host==Id(90)) assert(peer.last_seen==seen && !peer.metadata_ready && peer.endpoints.empty());
        // Force a lost response: the fake peer never answers requests. Real peers
        // must continue discovery/metadata normally with bounded independent state.
        assert(Has(a,2,n::PeerState::ONLINE,true,0));
        const auto stop_begin=std::chrono::steady_clock::now();c.Stop();c.Stop();assert(std::chrono::steady_clock::now()-stop_begin<1s);
        Until([&]{return Has(a,3,n::PeerState::LOST) && Has(b,3,n::PeerState::LOST);});
        std::uint64_t value=0,seq=0;assert(local.Publish(201)==0 && reader.ReadLatestSnapshot(value,seq)==0 && value==201 && seq==201);
        assert(reader.Close()==0 && local.Close()==0 && local.Unlink()==0);
        cc.profile.network.major++;
        assert(c.Start(cc,Metadata(3))==0);Until([&]{return Has(a,3,n::PeerState::ONLINE,true,4);});
        for(const auto& peer:a.GetPeers()) if(peer.host==Id(3))
            assert(peer.compatibility.reason==n::CompatibilityReason::NETWORK_MAJOR_MISMATCH);
        // exec after fork: no inherited mutex use in child of multithreaded test.
        const std::string number=std::to_string(port);const auto child=fork();assert(child>=0);
        if(!child){execl(executable,executable,"--child",number.c_str(),nullptr);_exit(127);}
        Until([&]{return Has(a,4,n::PeerState::ONLINE,true,4);});
        assert(kill(child,SIGKILL)==0);int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status));
        Until([&]{return Has(a,4,n::PeerState::LOST) && Has(b,4,n::PeerState::LOST);});
        assert(a.GetLastError()==0 && b.GetLastError()==0 && c.GetLastError()==0);
        a.Stop();b.Stop();c.Stop();assert(a.GetPeers().empty());assert(a.UpdateMetadata({})==-ENOTCONN);
        for(int i=0;i<10;++i) {assert(a.Start(ca)==0);a.Stop();}
        ca.multicast_group="127.0.0.1";assert(a.Start(ca)==-EINVAL);
    }
    assert(Fds()==baseline);
    std::cout<<"2/3 peers, self exclusion, revisions/chunks, duplicates, normal/crash loss, restart, Stop/FD cleanup PASS\n";
}
void Capacity() {
    const auto port=FreePort();n::PeerDiscovery observer;auto config=Config(1,port);config.max_peers=2;
    assert(observer.Start(config)==0);Injector inject(port);
    for(unsigned i=10;i<20;++i) {std::vector<std::uint8_t> bytes;assert(n::EncodeDiscoveryPacket(Announce(i),bytes)==0);inject.Send(bytes);}
    Until([&]{return observer.GetPeers().size()==2;});std::this_thread::sleep_for(100ms);assert(observer.GetPeers().size()==2);
    Until([&]{for(const auto& peer:observer.GetPeers())if(peer.state!=n::PeerState::LOST)return false;return true;});
    auto p=Announce(50);std::vector<std::uint8_t> bytes;assert(n::EncodeDiscoveryPacket(p,bytes)==0);inject.Send(bytes);
    Until([&]{return Has(observer,50,n::PeerState::ONLINE);});assert(observer.GetPeers().size()==2);
    observer.Stop();assert(observer.UpdateMetadata({})==-ENOTCONN);
    const auto fds=Fds();config.interface_address="192.0.2.123";assert(observer.Start(config)<0);observer.Stop();assert(Fds()==fds);
    std::cout<<"peer table capacity/LOST eviction/failed Start cleanup PASS\n";
}
void TransientRecovery() {
    const auto baseline=Fds();
    {
        const auto port=FreePort();auto config=Config(71,port);config.metadata_port=FreePort();
        n::PeerDiscovery observer,remote;
        assert(observer.Start(config,Metadata(71))==0);
        int send_fd=-1,receive_fd=-1;
        for(const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) {
            int fd=std::stoi(entry.path().filename().string());sockaddr_in a{};socklen_t size=sizeof(a);
            if(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size)==0 && a.sin_family==AF_INET) {
                if(ntohs(a.sin_port)==config.metadata_port) send_fd=fd;
                if(ntohs(a.sin_port)==port) receive_fd=fd;
            }
        }
        assert(send_fd>=0 && receive_fd>=0);
        assert(remote.Start(Config(72,port),Metadata(72))==0);
        Until([&]{return Has(observer,72,n::PeerState::ONLINE,true,4);});
        const auto session=observer.GetSessionId();
        failed_send_fd=send_fd;
        Until([&]{return observer.GetLastError()==-ENETUNREACH;});
        // Valid incoming announces, repeated queries and time alone cannot heal send failure.
        std::this_thread::sleep_for(150ms);
        assert(observer.GetLastError()==-ENETUNREACH);
        for(const auto& p:observer.GetPeers()) {assert(p.state==n::PeerState::LOST);assert(!p.metadata_ready && !p.last_complete_snapshot.valid);}
        failed_send_fd=-1;
        Until([&]{return observer.GetLastError()==0 && Has(observer,72,n::PeerState::ONLINE,true,4);});
        failed_receive_fd=receive_fd;
        Until([&]{return observer.GetLastError()==-EHOSTUNREACH;});
        // Announce sends still succeed, but must not clear the receive failure.
        std::this_thread::sleep_for(500ms);
        assert(observer.GetLastError()==-EHOSTUNREACH);
        assert(Has(observer,72,n::PeerState::LOST));
        failed_receive_fd=-1;
        Until([&]{return observer.GetLastError()==0 && Has(observer,72,n::PeerState::ONLINE,true,4);});
        for(const auto& p:observer.GetPeers()) assert(p.last_complete_snapshot.valid && !p.last_complete_snapshot.stale);
        assert(observer.GetSessionId()==session);
        injected_send_error=EBADF;failed_send_fd=send_fd;
        Until([&]{return observer.GetLastError()==-EBADF;});
        failed_send_fd=-1;injected_send_error=ENETUNREACH;
        std::this_thread::sleep_for(150ms);
        assert(observer.GetLastError()==-EBADF); // Valid remote traffic must not heal a fatal worker failure.
        for(const auto& p:observer.GetPeers()) {assert(p.state==n::PeerState::LOST);assert(!p.metadata_ready && !p.last_complete_snapshot.valid);}
        observer.Stop();assert(observer.GetPeers().empty());remote.Stop();
    }
    assert(Fds()==baseline);
    std::cout<<"transient send/receive errors: same-direction success, no query/time/opposite-direction clearing, metadata recovery/session/Stop PASS\n";
}
int main(int argc,char** argv) {
    if(argc==3 && std::string(argv[1])=="--child") {
        n::PeerDiscovery d;assert(d.Start(Config(4,std::stoi(argv[2])),Metadata(4))==0);for(;;)pause();
    }
    alarm(45);TransientRecovery();Codec();SnapshotRevisions();Integration(argv[0]);Capacity();
}
