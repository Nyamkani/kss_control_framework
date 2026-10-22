#include "kcf/network/peer_discovery.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <csignal>
#include <arpa/inet.h>
#include <unistd.h>

namespace n = kcf::network;
using R = n::CompatibilityReason;
using namespace std::chrono_literals;
n::HostIdentity Id(unsigned char value) { n::HostIdentity id; id.bytes[0]=value; return id; }
n::NetworkEndpointMetadata Endpoint(n::EndpointKind kind, unsigned char host, n::EndpointRole role)
{
    n::NetworkEndpointMetadata e;
    e.identity={{Id(host),Id(42),123,456},1};
    e.kind=kind;e.role=role;e.name="/test";e.element_name="test";e.scope=n::NetworkScope::REMOTE;
    const n::TypeIdentity type{1,2,3,4};
    if(kind==n::EndpointKind::TOPIC || kind==n::EndpointKind::PARAMETER) e.value_type=type;
    else {e.request_type=type;e.response_type=type;}
    if(kind==n::EndpointKind::ACTION) e.feedback_type=type;
    return e;
}
void Expect(n::CompatibilityResult result, R reason) { assert(result.reason==reason);assert(result.Compatible()==(reason==R::COMPATIBLE)); }
void Contracts()
{
    n::PeerProfile a,b;
    Expect(n::CheckPeerCompatibility(a,b),R::COMPATIBLE);
    Expect(n::CheckPeerCompatibility(a,b,n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION),R::COMPATIBLE);
    assert(n::CheckPeerCompatibility(a,b).common_capabilities==n::CapabilityBit(n::Capability::DISCOVERY));
    b.framework.major++;
    Expect(n::CheckPeerCompatibility(a,b),R::COMPATIBLE);
    Expect(n::CheckPeerCompatibility(a,b,n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION),R::FRAMEWORK_VERSION_MISMATCH);
    b=a;b.framework.minor++;
    Expect(n::CheckPeerCompatibility(a,b,n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION),R::FRAMEWORK_VERSION_MISMATCH);
    b=a;b.framework.patch++;
    Expect(n::CheckPeerCompatibility(a,b,n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION),R::FRAMEWORK_VERSION_MISMATCH);
    b=a;b.network.major++;
    Expect(n::CheckPeerCompatibility(a,b),R::NETWORK_MAJOR_MISMATCH);
    Expect(n::CheckPeerCompatibility(a,b,n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION),R::NETWORK_MAJOR_MISMATCH);
    b.network.major=0;Expect(n::CheckPeerCompatibility(a,b),R::INVALID_PROFILE);
    b=a;Expect(n::CheckPeerCompatibility(a,b,static_cast<n::CompatibilityPolicy>(0)),R::INVALID_POLICY);
    a.capabilities=n::KNOWN_CAPABILITIES;b=a;b.network.minor++;
    b.capabilities &= ~n::CapabilityBit(n::Capability::REMOTE_ACTION);
    auto result=n::CheckPeerCompatibility(a,b);Expect(result,R::COMPATIBLE);
    assert(result.common_capabilities==b.capabilities);
    for(auto cap:{n::Capability::REMOTE_PARAMETER,n::Capability::REMOTE_SERVICE,n::Capability::REMOTE_TOPIC})
        Expect(n::CheckCapability(a,b,n::CapabilityBit(cap)),R::COMPATIBLE);
    result=n::CheckCapability(a,b,n::CapabilityBit(n::Capability::REMOTE_ACTION));
    Expect(result,R::CAPABILITY_MISSING);assert(result.missing_capabilities==n::CapabilityBit(n::Capability::REMOTE_ACTION));
    const auto unknown=1ull<<63;a.capabilities|=unknown;b.capabilities|=unknown;
    assert(!(n::CheckPeerCompatibility(a,b).common_capabilities&unknown));
    Expect(n::CheckCapability(a,b,unknown),R::CAPABILITY_MISSING);
    a.capabilities=n::KNOWN_CAPABILITIES;b=a;
    for(auto kind:{n::EndpointKind::TOPIC,n::EndpointKind::PARAMETER,n::EndpointKind::SERVICE,n::EndpointKind::ACTION}) {
        auto provider=Endpoint(kind,1,n::EndpointRole::PROVIDER);
        auto consumer=Endpoint(kind,2,n::EndpointRole::CONSUMER);
        Expect(n::CheckEndpointCompatibility(a,b,provider,consumer),R::COMPATIBLE);
        Expect(n::CheckEndpointCompatibility(a,b,consumer,provider),R::COMPATIBLE);
        Expect(n::CheckEndpointCompatibility({}, {},provider,consumer),R::CAPABILITY_MISSING);
        auto changed=consumer;changed.name="/other";Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::SEMANTIC_MISMATCH);
        changed=consumer;changed.role=n::EndpointRole::PROVIDER;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::SEMANTIC_MISMATCH);
        changed=consumer;changed.scope=n::NetworkScope::LOCAL;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::LOCAL_ONLY);
        changed=consumer;changed.identity.runtime.host=Id(1);Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::HOST_MISMATCH);
        changed=consumer;changed.identity.registration_id=0;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::INVALID_ENDPOINT);
        for(auto member:{&n::NetworkEndpointMetadata::value_type,&n::NetworkEndpointMetadata::request_type,
                         &n::NetworkEndpointMetadata::response_type,&n::NetworkEndpointMetadata::feedback_type}) {
            if(!(consumer.*member).type_id) continue;
            changed=consumer;(changed.*member).type_id++;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::TYPE_MISMATCH);
            changed=consumer;(changed.*member).schema_id++;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::TYPE_MISMATCH);
            changed=consumer;(changed.*member).encoding_id++;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::TYPE_MISMATCH);
            changed=consumer;(changed.*member).payload_size++;Expect(n::CheckEndpointCompatibility(a,b,provider,changed),R::TYPE_MISMATCH);
        }
        for(auto scope:{n::TargetScope::ENDPOINT,n::TargetScope::HOST,n::TargetScope::APPLICATION,n::TargetScope::GROUP,n::TargetScope::ALL}) {
            Expect(n::CheckEndpointCompatibility(a,b,provider,consumer,n::CompatibilityPolicy::NETWORK_PROTOCOL,scope),R::COMPATIBLE);
            auto limited=b;limited.capabilities &= ~n::TargetCapability(scope);
            Expect(n::CheckEndpointCompatibility(a,limited,provider,consumer,n::CompatibilityPolicy::NETWORK_PROTOCOL,scope),R::CAPABILITY_MISSING);
        }
        Expect(n::CheckEndpointCompatibility(a,b,provider,consumer,n::CompatibilityPolicy::NETWORK_PROTOCOL,static_cast<n::TargetScope>(0)),R::SEMANTIC_MISMATCH);
        if(kind==n::EndpointKind::TOPIC) Expect(n::CheckTopicCompatibility(a,b,provider,consumer),R::COMPATIBLE);
        if(kind==n::EndpointKind::PARAMETER) Expect(n::CheckParameterCompatibility(a,b,provider,consumer),R::COMPATIBLE);
        if(kind==n::EndpointKind::SERVICE) Expect(n::CheckServiceCompatibility(a,b,provider,consumer),R::COMPATIBLE);
        if(kind==n::EndpointKind::ACTION) Expect(n::CheckActionCompatibility(a,b,provider,consumer),R::COMPATIBLE);
    }
    Expect(n::CheckTopicCompatibility(a,b,Endpoint(n::EndpointKind::SERVICE,1,n::EndpointRole::PROVIDER),
        Endpoint(n::EndpointKind::SERVICE,2,n::EndpointRole::CONSUMER)),R::SEMANTIC_MISMATCH);
    std::cout<<"version/policy/capability intersection/endpoint semantics/all type fields PASS\n";
}
std::uint16_t Port()
{
    int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in address{};
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_t size=sizeof(address);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)==0);
    close(fd);return ntohs(address.sin_port);
}
template<class F> void Until(F condition)
{
    auto end=std::chrono::steady_clock::now()+5s;
    while(!condition()) {assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}
}
n::PeerInfo Peer(n::PeerDiscovery& discovery, unsigned char host)
{
    for(const auto& p:discovery.GetPeers()) if(p.host==Id(host)) return p;
    assert(false);return {};
}
void Discovery()
{
    n::DiscoveryConfig ca;ca.host=Id(1);ca.boot_id=Id(42);ca.interface_address="127.0.0.1";ca.port=Port();
    ca.announce_interval=60ms;ca.peer_timeout=400ms;ca.metadata_retry=40ms;
    auto cb=ca;cb.host=Id(2);cb.profile.network.major++;
    auto cc=ca;cc.host=Id(3);cc.profile.framework.patch++;cc.compatibility_policy=n::CompatibilityPolicy::EXACT_FRAMEWORK_VERSION;
    n::PeerDiscovery a,b,c;
    auto local=Endpoint(n::EndpointKind::TOPIC,1,n::EndpointRole::CONSUMER);
    auto remote=Endpoint(n::EndpointKind::TOPIC,2,n::EndpointRole::PROVIDER);
    auto third=Endpoint(n::EndpointKind::TOPIC,3,n::EndpointRole::PROVIDER);
    assert(a.Start(ca,{local})==0 && b.Start(cb,{remote})==0 && c.Start(cc,{third})==0);
    Until([&]{for(auto d:{&a,&b,&c}) {
        auto peers=d->GetPeers();if(peers.size()!=2) return false;
        for(const auto& p:peers)if(!p.metadata_ready || p.state!=n::PeerState::ONLINE)return false;
    }return true;});
    auto pb=Peer(a,2);Expect(pb.compatibility,R::NETWORK_MAJOR_MISMATCH);assert(pb.profile.network.major==2);
    Expect(n::CheckRouteCompatibility(ca,pb,local,remote.identity),R::NETWORK_MAJOR_MISMATCH);
    Expect(Peer(b,1).compatibility,R::NETWORK_MAJOR_MISMATCH);
    auto pc=Peer(a,3);Expect(pc.compatibility,R::COMPATIBLE);assert(pc.profile.framework.patch==1);
    Expect(Peer(c,1).compatibility,R::FRAMEWORK_VERSION_MISMATCH);
    Expect(n::CheckRouteCompatibility(ca,pc,local,third.identity),R::CAPABILITY_MISSING);
    // Synthetic supported-transport profiles exercise the future route gate.
    auto supported=ca;supported.profile.capabilities=n::KNOWN_CAPABILITIES;
    pc.profile.capabilities=n::KNOWN_CAPABILITIES;
    Expect(n::CheckRouteCompatibility(supported,pc,local,third.identity),R::COMPATIBLE);
    auto wrong=third.identity;wrong.registration_id++;
    Expect(n::CheckRouteCompatibility(supported,pc,local,wrong),R::UNKNOWN_ENDPOINT);
    wrong=third.identity;wrong.runtime.host=Id(4);
    Expect(n::CheckRouteCompatibility(supported,pc,local,wrong),R::HOST_MISMATCH);
    pc.metadata_ready=false;Expect(n::CheckRouteCompatibility(supported,pc,local,third.identity),R::METADATA_UNAVAILABLE);
    pc.metadata_ready=true;pc.endpoints[0].value_type.type_id++;
    Expect(n::CheckRouteCompatibility(supported,pc,local,third.identity),R::TYPE_MISMATCH);
    c.Stop();b.Stop();
    Until([&]{return Peer(a,2).state==n::PeerState::LOST && Peer(a,3).state==n::PeerState::LOST;});
    Expect(Peer(a,2).compatibility,R::NETWORK_MAJOR_MISMATCH);
    Expect(Peer(a,3).compatibility,R::COMPATIBLE);
    Expect(n::CheckRouteCompatibility(ca,Peer(a,2),local,remote.identity),R::PEER_LOST);
    Expect(n::CheckRouteCompatibility(ca,Peer(a,3),local,third.identity),R::PEER_LOST);
    a.Stop();
    auto invalid=ca;invalid.profile.network.major=0;assert(a.Start(invalid)==-EINVAL);
    invalid=ca;invalid.profile.capabilities=0;assert(a.Start(invalid)==-EINVAL);
    invalid=ca;invalid.compatibility_policy=static_cast<n::CompatibilityPolicy>(0);assert(a.Start(invalid)==-EINVAL);
    std::cout<<"incompatible peers remain discovered/metadata visible/independent LOST/route gate PASS\n";
}
int main() { alarm(25);Contracts();Discovery(); }
