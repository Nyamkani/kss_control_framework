#include "kcf/network/peer_discovery.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <random>
#include <csignal>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <unistd.h>
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
void Codec() {
    std::vector<std::uint8_t> bytes;auto original=Announce();assert(n::EncodeDiscoveryPacket(original,bytes)==0);
    assert(bytes.size()==100 && bytes[0]=='K' && bytes[1]=='C' && bytes[2]=='F' && bytes[3]=='D');
    n::DiscoveryPacket decoded;assert(n::DecodeDiscoveryPacket(bytes.data(),bytes.size(),decoded)==0 && decoded.host==original.host);
    for(std::size_t i=0;i<bytes.size();++i) {
        decoded.host=Id(99);assert(n::DecodeDiscoveryPacket(bytes.data(),i,decoded)!=0 && decoded.host==Id(99));
    }
    assert(bytes[5]==2 && bytes[11]==100 && bytes[81]==5 && bytes[83]==1 && bytes[87]==1 && bytes[99]==1);
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
int main(int argc,char** argv) {
    if(argc==3 && std::string(argv[1])=="--child") {
        n::PeerDiscovery d;assert(d.Start(Config(4,std::stoi(argv[2])),Metadata(4))==0);for(;;)pause();
    }
    alarm(45);Codec();Integration(argv[0]);Capacity();
}
