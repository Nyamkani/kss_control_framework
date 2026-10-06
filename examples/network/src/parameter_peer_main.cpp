#include "kcf/network/control_node.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/service/service_server.hpp"
#include <arpa/inet.h>
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace n=kcf::network;
volatile std::sig_atomic_t stopped=0;
void Signal(int){stopped=1;}
bool Id(const std::string& text,n::HostIdentity& id) {
    std::string s;for(char c:text)if(c!='-')s+=c;
    if(s.size()!=32)return false;
    auto hex=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(unsigned i=0;i<16;++i){int a=hex(s[2*i]),b=hex(s[2*i+1]);if(a<0||b<0)return false;id.bytes[i]=(a<<4)|b;}
    return n::Valid(id);
}
int main(int argc,char** argv) {
    if(argc!=6&&argc!=8){std::cerr<<"Usage: kcf_parameter_peer HOST_HEX IPV4 DISCOVERY_PORT owner NAME\n"
        <<"   or: kcf_parameter_peer HOST_HEX IPV4 DISCOVERY_PORT client NAME REMOTE_HOST_HEX NEW_COUNT\nService: replace owner/client with service-owner/service-client (count is request).\n";return 2;}
    n::ControlConfig config;std::string boot;std::ifstream("/proc/sys/kernel/random/boot_id")>>boot;
    if(!Id(argv[1],config.discovery.host)||!Id(boot,config.discovery.boot_id))return 2;
    config.discovery.interface_address=argv[2];config.metadata.runtime_pids={getpid()};
    int count=0;
    try{std::size_t used=0;auto port=std::stoul(argv[3],&used);if(used!=std::strlen(argv[3])||!port||port>65535)return 2;config.discovery.port=port;
        if(argc==8){count=std::stoi(argv[7],&used);if(used!=std::strlen(argv[7]))return 2;}}
    catch(...){return 2;}
    const std::string mode=argv[4];
    const bool service=mode=="service-owner"||mode=="service-client";
    const bool owner=mode=="owner"||mode=="service-owner";
    if((owner&&argc!=6)||(!owner&&((mode!="client"&&mode!="service-client")||argc!=8)))return 2;
    n::RemoteTarget target;target.scope=n::TargetScope::HOST;if(!owner&&!Id(argv[6],target.host))return 2;
    std::signal(SIGINT,Signal);std::signal(SIGTERM,Signal);
    kcf::Parameter<Value> parameter;kcf::ServiceServer server;
    if(owner){if(service)config.metadata.remote_services={argv[5]};else config.metadata.remote_parameters={argv[5]};kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
        int code=0;
        if(service){
            int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 1;
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){close(fd);return 1;}
            socklen_t size=sizeof(address);if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size)){close(fd);return 1;}close(fd);
            code=server.Create(ntohs(address.sin_port));
            if(!code)code=server.Register<Value,Value>(1,argv[5],[](const Value& request,Value& response){response={request.count*2,!request.enabled,request.gain*2};});
            if(!code)code=server.Start();
        }else code=parameter.Create(argv[5],{7,true,1.5});if(code){std::cerr<<"Create="<<code<<'\n';return 1;}
        kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);}
    n::ControlNode node;int code=node.Start(config);if(code){std::cerr<<"Start="<<code<<'\n';if(owner){if(service)server.Stop();else{parameter.Close();parameter.Unlink();}kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,code);}return 1;}
    int result=0;
    if(owner){std::cout<<(service?"Service owner ready: ":"Parameter owner ready: ")<<argv[5]<<std::endl;while(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds(50));}
    else {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);bool found=false;
        while(!stopped&&std::chrono::steady_clock::now()<deadline&&!found){for(const auto& peer:node.GetPeers())if(peer.host==target.host&&peer.metadata_ready)found=true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}
        auto descriptor=kcf::TypeDescriptorTraits<Value>::Get();n::TypeIdentity type;n::ParameterWireType(descriptor,type);
        auto display=[&](n::ParameterBatchResult batch){if(batch.error||batch.endpoints.empty()){std::cerr<<"selection error="<<batch.error<<'\n';return false;}
            bool good=true;for(const auto& endpoint:batch.endpoints){std::cout<<"error="<<endpoint.error<<" reason="<<n::CompatibilityReasonName(endpoint.reason)<<" execution_unknown="<<endpoint.execution_unknown;
                if(!endpoint.error&&!endpoint.value.empty()){kcf::DynamicPayload decoded;if(n::DecodeParameterValue(descriptor,endpoint.value,decoded)){good=false;}else{Value v;std::memcpy(&v,decoded.bytes.data(),sizeof(v));std::cout<<" count="<<v.count<<" gain="<<v.gain;}}
                std::cout<<'\n';good=good&&!endpoint.error;}return good;};
        if(service){
            Value value{count,true,1.5};kcf::DynamicPayload payload;payload.type_id=type.type_id;payload.bytes.resize(sizeof(value));std::memcpy(payload.bytes.data(),&value,sizeof(value));
            std::vector<std::uint8_t> wire;n::EncodeParameterValue(descriptor,payload,wire);
            if(!found||!display(node.CallService(target,argv[5],type,type,wire,std::chrono::seconds(3)).get()))result=1;
        }
        else if(!found||!display(node.Get(target,argv[5],type,std::chrono::seconds(3)).get()))result=1;
        else {Value v{count,true,2.5};kcf::DynamicPayload payload;payload.type_id=type.type_id;payload.bytes.resize(sizeof(v));std::memcpy(payload.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> wire;n::EncodeParameterValue(descriptor,payload,wire);
            if(!display(node.Set(target,argv[5],type,wire,std::chrono::seconds(3)).get())||!display(node.Get(target,argv[5],type,std::chrono::seconds(3)).get()))result=1;}
    }
    node.Stop();if(owner){if(service)server.Stop();else{parameter.Close();parameter.Unlink();}kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);}return result;
}
