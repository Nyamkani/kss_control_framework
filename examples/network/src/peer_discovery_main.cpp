#include "kcf/network/peer_discovery.hpp"
#include "kcf/network/validation.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace n=kcf::network;
namespace {
volatile std::sig_atomic_t stopping=0;
void Signal(int) {stopping=1;}
bool ParseId(const std::string& input,n::HostIdentity& output) {
    std::string text;for(char c:input)if(c!='-')text+=c;
    if(text.size()!=32)return false;
    auto hex=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(unsigned i=0;i<16;++i){int a=hex(text[2*i]),b=hex(text[2*i+1]);if(a<0||b<0)return false;output.bytes[i]=(a<<4)|b;}
    return n::Valid(output);
}
std::string Hex(const n::HostIdentity& id) {
    std::string s;for(auto byte:id.bytes){s+="0123456789abcdef"[byte>>4];s+="0123456789abcdef"[byte&15];}return s;
}
}
int main(int argc,char** argv) {
    if(argc<3||argc>5){std::cerr<<"Usage: kcf_peer_discovery HOST_HEX INTERFACE_IPV4 [PORT] [ANNOUNCE_MS]\n";return 2;}
    n::DiscoveryConfig config;std::string boot;std::ifstream("/proc/sys/kernel/random/boot_id")>>boot;
    if(!ParseId(argv[1],config.host)||!ParseId(boot,config.boot_id)){std::cerr<<"Invalid host/boot identity\n";return 2;}
    config.interface_address=argv[2];
    try {
        if(argc>3){std::size_t n;auto p=std::stoul(argv[3],&n);if(n!=std::string(argv[3]).size()||p<1||p>65535)return 2;config.port=p;}
        if(argc>4){std::size_t n;auto ms=std::stoul(argv[4],&n);if(n!=std::string(argv[4]).size()||ms<20||ms>60000)return 2;config.announce_interval=std::chrono::milliseconds(ms);config.peer_timeout=config.announce_interval*4;}
    } catch(...){return 2;}
    if(config.metadata_retry>config.peer_timeout) config.metadata_retry=config.announce_interval;
    std::uint64_t ticks=0;if(!kcf::detail::ReadProcessIdentity(getpid(),ticks))return 1;
    std::vector<n::NetworkEndpointMetadata> metadata;
    const char* names[]{"topic","parameter","service","action"};
    for(unsigned i=0;i<4;++i){
        n::NetworkEndpointMetadata m;m.identity={{config.host,config.boot_id,getpid(),ticks},i+1};m.element_name="discovery_demo";
        m.name=std::string("/example/discovery/")+names[i];m.kind=static_cast<n::EndpointKind>(i+1);m.scope=n::NetworkScope::REMOTE;
        const n::TypeIdentity type{1,1,1,4}; // demo declaration, NOT an executable endpoint or payload codec
        if(i<2)m.value_type=type;else{m.request_type=type;m.response_type=type;}
        if(i==3)m.feedback_type=type;
        metadata.push_back(m);
    }
    n::PeerDiscovery discovery;int result=discovery.Start(config,metadata);
    if(result){std::cerr<<"Discovery Start error="<<result<<'\n';return 1;}
    std::signal(SIGINT,Signal);std::signal(SIGTERM,Signal);
    std::cout<<"Discovery started host="<<Hex(config.host)<<" (metadata demo only; no remote operations)"<<std::endl;
    while(!stopping){
        for(const auto& peer:discovery.GetPeers()){
            std::cout<<"peer="<<Hex(peer.host)<<" address="<<peer.address<<":"<<peer.port
                <<" state="<<(peer.state==n::PeerState::ONLINE?"ONLINE":"LOST")<<" revision="<<peer.metadata_revision
                <<" compatibility="<<n::CompatibilityReasonName(peer.compatibility.reason)
                <<" metadata="<<(peer.metadata_ready?"ready":"pending")<<" endpoints="<<peer.endpoints.size()<<std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    discovery.Stop();std::cout<<"Discovery stopped"<<std::endl;
}
