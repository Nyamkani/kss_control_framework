#include "kcf/network/control_node.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/detail/supervisor_registry.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>
#include <random>
#include <filesystem>
#include <csignal>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std::chrono_literals;
namespace n=kcf::network;
#include "parameter_demo_type.hpp"
auto Descriptor(){return kcf::TypeDescriptorTraits<Value>::Get();}
n::TypeIdentity Type(){n::TypeIdentity t;assert(n::ParameterWireType(Descriptor(),t)==0);return t;}
std::vector<std::uint8_t> Encode(Value v){kcf::DynamicPayload p;p.type_id=Type().type_id;p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> out;assert(n::EncodeParameterValue(Descriptor(),p,out)==0);return out;}
Value Decode(const std::vector<std::uint8_t>& bytes){kcf::DynamicPayload p;assert(n::DecodeParameterValue(Descriptor(),bytes,p)==0);Value v;std::memcpy(&v,p.bytes.data(),sizeof(v));return v;}
n::HostIdentity Id(unsigned char x){n::HostIdentity id;id.bytes[0]=x;return id;}
template<class F> void Until(F condition){auto end=std::chrono::steady_clock::now()+8s;while(!condition()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
std::uint16_t Port(int kind=SOCK_DGRAM,int* keep=nullptr){int fd=socket(AF_INET,kind|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t size=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size)==0);if(keep)*keep=fd;else close(fd);return ntohs(a.sin_port);}
n::ControlConfig Config(unsigned char host,std::uint16_t port){n::ControlConfig c;c.discovery.host=Id(host);c.discovery.boot_id=Id(42);c.discovery.port=port;c.discovery.interface_address="127.0.0.1";c.discovery.announce_interval=60ms;c.discovery.peer_timeout=400ms;c.discovery.metadata_retry=40ms;c.metadata_interval=40ms;c.connect_timeout=300ms;c.reconnect_interval=50ms;c.metadata.runtime_pids={-1};return c;}
n::PeerInfo Peer(n::ControlNode& node,unsigned char host){for(const auto& p:node.GetPeers())if(p.host==Id(host))return p;return {};}
n::RemoteTarget Host(unsigned char id){n::RemoteTarget t;t.scope=n::TargetScope::HOST;t.host=Id(id);return t;}
n::ParameterBatchResult Result(std::future<n::ParameterBatchResult> f){assert(f.wait_for(5s)==std::future_status::ready);return f.get();}
n::ParameterEndpointResult One(std::future<n::ParameterBatchResult> f){auto r=Result(std::move(f));if(r.error)std::cerr<<"batch error "<<r.error<<'\n';assert(!r.error&&r.endpoints.size()==1);return r.endpoints[0];}
void SendBytes(int fd,const std::vector<std::uint8_t>& bytes,bool partial=false){std::size_t offset=0;while(offset<bytes.size()){auto n=send(fd,bytes.data()+offset,partial?1:bytes.size()-offset,MSG_NOSIGNAL);assert(n>0);offset+=n;if(partial)std::this_thread::sleep_for(100us);}}
void Send(int fd,const n::ControlFrame& frame,bool partial=false){std::vector<std::uint8_t> bytes;assert(n::EncodeControlFrame(frame,bytes)==0);SendBytes(fd,bytes,partial);}
n::ControlFrame Receive(int fd){std::vector<std::uint8_t> bytes;std::size_t size=n::CONTROL_PREFIX_SIZE;while(bytes.size()<size){pollfd p{fd,POLLIN,0};assert(poll(&p,1,3000)>0);std::uint8_t b[4096];auto n=recv(fd,b,std::min(sizeof(b),size-bytes.size()),0);assert(n>0);bytes.insert(bytes.end(),b,b+n);if(bytes.size()==n::CONTROL_PREFIX_SIZE)assert(n::ControlFrameSize(bytes.data(),bytes.size(),size)==0);}n::ControlFrame f;assert(n::DecodeControlFrame(bytes.data(),bytes.size(),f)==0);return f;}
int Connect(std::uint16_t port){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(port);assert(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);return fd;}
n::NetworkEndpointMetadata Fake(unsigned char host,const std::string& name){n::NetworkEndpointMetadata m;m.identity={{Id(host),Id(42),getpid(),1},1};m.kind=n::EndpointKind::PARAMETER;m.scope=n::NetworkScope::REMOTE;m.element_name="fake";m.name=name;m.value_type=Type();return m;}
void Codec(){auto t=Type();assert(t.payload_size==13);auto bytes=Encode({-2,true,1.5});assert(bytes.size()==13&&bytes[0]==255&&bytes[3]==254&&bytes[4]==1);auto v=Decode(bytes);assert(v.count==-2&&v.enabled&&v.gain==1.5);
    bytes[4]=2;kcf::DynamicPayload p;assert(n::DecodeParameterValue(Descriptor(),bytes,p)==-EPROTO);
    auto d=Descriptor();d.fields[0].kind=kcf::FieldValueKind::UINT32;n::TypeIdentity changed;assert(n::ParameterWireType(d,changed)==0&&changed.schema_id!=t.schema_id);
    n::ControlFrame f;f.source={Id(1),Id(42),getpid(),1};f.session_id=1;assert(n::EncodeControlFrame(f,bytes)==0&&bytes.size()==172);
    n::ControlFrame decoded;assert(n::DecodeControlFrame(bytes.data(),bytes.size(),decoded)==0);
    for(std::size_t i=0;i<bytes.size();++i)assert(n::DecodeControlFrame(bytes.data(),i,decoded)!=0);
    for(auto offset:{0,5,7,11,15}){auto bad=bytes;bad[offset]^=255;assert(n::DecodeControlFrame(bad.data(),bad.size(),decoded)!=0);}
    auto bad=bytes;bad.push_back(0);assert(n::DecodeControlFrame(bad.data(),bad.size(),decoded)!=0);
    std::mt19937 random(77);for(int i=0;i<3000;++i){bad.resize(random()%300);for(auto& b:bad)b=random();(void)n::DecodeControlFrame(bad.data(),bad.size(),decoded);}
    std::cout<<"portable field codec, padding excluded, framing bounds/malformed PASS\n";
}
volatile std::sig_atomic_t stop_owner=0;void StopOwner(int){stop_owner=1;}
int Owner(const char* name){signal(SIGTERM,StopOwner);kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::Parameter<Value> parameter;assert(parameter.Create(name,{7,true,1.5})==0);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);while(!stop_owner)std::this_thread::sleep_for(10ms);parameter.Close();parameter.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);return 0;}
void Basic(const char* executable){
    const auto port=Port();std::string name="/kcf_network_"+std::to_string(getpid());
    const auto child=fork();assert(child>=0);if(!child){execl(executable,executable,"--owner",name.c_str(),nullptr);_exit(127);}
    kcf::detail::SupervisorRegistry supervisor;supervisor.Begin();
    kcf::SupervisorInfo app{};std::strcpy(app.application_name,"network_test");app.application_state=kcf::ApplicationState::RUNNING;
    app.element_count=1;app.elements[0].pid=child;std::strcpy(app.elements[0].name,"parameter_owner");
    std::strcpy(app.elements[0].executable,executable);app.elements[0].process_alive=1;
    Until([&]{return kcf::detail::ReadProcessIdentity(child,app.elements[0].process_start_ticks);});supervisor.Update(app);
    n::ControlNode a,b,c;auto ca=Config(10,port),cb=Config(20,port),cc=Config(30,port);ca.metadata.runtime_pids={child};ca.metadata.remote_parameters={name};ca.metadata.groups={"robots"};cc.metadata=ca.metadata;
    assert(a.Start(ca)==0&&b.Start(cb)==0&&c.Start(cc)==0);
    Until([&]{auto p=Peer(b,10);auto q=Peer(b,30);return p.metadata_ready&&p.endpoints.size()==1&&q.metadata_ready&&q.endpoints.size()==1;});
    auto peer=Peer(b,10);assert(peer.endpoints[0].application_name=="network_test"&&peer.endpoints[0].element_name=="parameter_owner");assert(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_PARAMETER));assert(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_SERVICE));
    auto first=One(b.Get(Host(10),name,Type(),2s));assert(!first.error&&Decode(first.value).count==7);
    n::RemoteTarget endpoint;endpoint.endpoint=peer.endpoints[0].identity;
    auto set=One(b.Set(endpoint,name,Type(),Encode({99,false,4.5}),2s));assert(!set.error);
    auto got=One(b.Get(endpoint,name,Type()));assert(!got.error&&Decode(got.value).count==99);
    std::vector<std::future<n::ParameterBatchResult>> requests;for(int i=0;i<40;++i)requests.push_back(b.Get(Host(10),name,Type(),2s));
    for(auto& f:requests){auto r=One(std::move(f));assert(!r.error&&Decode(r.value).count==99);}
    n::RemoteTarget all;all.scope=n::TargetScope::ALL;auto both=Result(b.Set(all,name,Type(),Encode({123,true,9}),2s));assert(!both.error&&both.endpoints.size()==2);for(const auto& r:both.endpoints)assert(!r.error);
    n::RemoteTarget group;group.scope=n::TargetScope::GROUP;group.group="robots";both=Result(b.Get(group,name,Type()));assert(both.endpoints.size()==2);for(const auto& r:both.endpoints)assert(!r.error&&Decode(r.value).count==123);
    const auto sent=b.GetStats().requests_sent;auto wrong=Type();wrong.type_id++;auto denied=One(b.Get(endpoint,name,wrong));assert(denied.reason==n::CompatibilityReason::TYPE_MISMATCH&&b.GetStats().requests_sent==sent);
    // Metadata registration/removal is automatic, no Network registration in owner.
    assert(kill(child,SIGTERM)==0);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    Until([&]{auto p=Peer(b,10);return p.metadata_ready&&p.endpoints.empty()&&p.metadata_revision>peer.metadata_revision;});
    assert(Result(b.Get(endpoint,name,Type())).error);
    a.Stop();Until([&]{return Peer(b,10).state==n::PeerState::LOST;});assert(Result(b.Get(Host(10),name,Type())).error==-ENOTCONN);
    assert(a.Start(ca)==0);Until([&]{return Peer(b,10).state==n::PeerState::ONLINE&&Peer(b,10).metadata_ready;});
    b.Stop();c.Stop();a.Stop();supervisor.End();
    std::cout<<"auto metadata, separate owner process, Get/Set, ENDPOINT/HOST/GROUP/ALL, concurrency, removal/LOST/restart PASS\n";
}
void Failures(){
    const auto port=Port();const std::string name="/kcf_control_fail_"+std::to_string(getpid());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::Parameter<Value> owner;assert(owner.Create(name,{5,true,2})==0);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlNode a,b;auto ca=Config(10,port),cb=Config(20,port);ca.metadata.runtime_pids={getpid()};assert(a.Start(ca)==0&&b.Start(cb)==0);
    Until([&]{auto p=Peer(b,10);return p.metadata_ready&&!p.endpoints.empty();});auto denied=One(b.Get(Host(10),name,Type()));assert(denied.reason==n::CompatibilityReason::LOCAL_ONLY);
    a.Stop();ca.metadata.remote_parameters={name};assert(a.Start(ca)==0);
    Until([&]{auto p=Peer(b,10);return p.metadata_ready&&p.endpoints.size()==1&&p.endpoints[0].scope==n::NetworkScope::REMOTE;});
    n::PeerDiscovery missing,incompatible,unreachable;
    auto cm=Config(40,port).discovery;auto ci=Config(50,port).discovery;ci.profile.network.major=2;
    auto cu=Config(60,port).discovery;cu.profile.capabilities=n::KNOWN_CAPABILITIES;
    assert(missing.Start(cm,{Fake(40,name)})==0&&incompatible.Start(ci,{Fake(50,name)})==0&&unreachable.Start(cu,{Fake(60,name)})==0);
    Until([&]{return Peer(b,40).metadata_ready&&Peer(b,50).metadata_ready&&Peer(b,60).metadata_ready;});
    denied=One(b.Get(Host(40),name,Type()));assert(denied.reason==n::CompatibilityReason::CAPABILITY_MISSING);
    denied=One(b.Get(Host(50),name,Type()));assert(denied.reason==n::CompatibilityReason::NETWORK_MAJOR_MISMATCH);
    auto timeout=One(b.Get(Host(60),name,Type(),150ms));assert(timeout.error==-ETIMEDOUT);
    assert(owner.Set({6,false,3})==0);Value v;assert(owner.Get(v)==0&&v.count==6);
    n::RemoteTarget all;all.scope=n::TargetScope::ALL;auto results=Result(b.Get(all,name,Type(),150ms));assert(results.endpoints.size()==4);unsigned successes=0;for(const auto& r:results.endpoints)if(!r.error)++successes;assert(successes==1);
    auto future=b.Get(Host(60),name,Type(),10s);b.Stop();auto stopped=Result(std::move(future));assert(stopped.error||(!stopped.endpoints.empty()&&stopped.endpoints[0].error));
    a.Stop();missing.Stop();incompatible.Stop();unreachable.Stop();assert(owner.Get(v)==0&&v.count==6);owner.Close();owner.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"LOCAL/type/capability/version gate, connect failure/timeout, partial fan-out results, Local isolation/Stop PASS\n";
}

void OwnerBinding(const char* executable){
    const std::string name="/kcf_control_binding_"+std::to_string(getpid());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> owner;assert(owner.Create(name,{19,true,2})==0);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::MetadataConfig config;config.host=Id(10);config.boot_id=Id(42);config.runtime_pids={getpid()};
    std::vector<n::LocalEndpoint> entries;assert(n::CollectLocalMetadata(config,entries)==0&&entries.size()==1);
    assert(n::ValidateLocalParameterEndpoint(entries[0])==0);assert(owner.Unlink()==0);
    const auto child=fork();assert(child>=0);if(!child){execl(executable,executable,"--owner",name.c_str(),nullptr);_exit(127);}
    Until([&]{return n::ValidateLocalParameterEndpoint(entries[0])==-ESTALE;});
    Value value;assert(owner.Get(value)==0&&value.count==19);
    assert(kill(child,SIGTERM)==0);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(owner.Close()==0);assert(n::ValidateLocalParameterEndpoint(entries[0])==-ESTALE);
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"stale registration and replaced named storage owner rejected PASS\n";
}
void IncomingFrames(){
    const auto port=Port();const std::string name="/kcf_control_raw_"+std::to_string(getpid());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::Parameter<Value> owner;
    assert(owner.Create(name,{8,true,2})==0);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlNode node;auto config=Config(10,port);config.metadata.runtime_pids={getpid()};config.metadata.remote_parameters={name};assert(node.Start(config)==0);
    n::PeerDiscovery raw;auto raw_config=Config(1,port).discovery;raw_config.profile.capabilities=n::KNOWN_CAPABILITIES;assert(raw.Start(raw_config)==0);
    n::PeerInfo server;
    Until([&]{for(const auto& peer:raw.GetPeers())if(peer.host==Id(10)&&peer.metadata_ready&&peer.endpoints.size()==1){server=peer;return true;}return false;});
    Until([&]{return Peer(node,1).state==n::PeerState::ONLINE&&Peer(node,1).host==Id(1);});
    n::ControlFrame hello;hello.source={Id(1),Id(42),getpid(),1};hello.session_id=raw.GetSessionId();
    int fd=Connect(server.port);Send(fd,hello,true);assert(Receive(fd).operation==n::ControlMessage::HELLO);
    n::ControlFrame request=hello;request.operation=n::ControlMessage::PARAM_GET_REQUEST;request.request_id=1;
    request.endpoint=server.endpoints[0].identity;request.name=name;request.type=Type();request.timeout_ms=1000;
    Send(fd,request,true);auto response=Receive(fd);assert(!response.status&&Decode(response.payload).count==8);
    request.operation=n::ControlMessage::PARAM_SET_REQUEST;request.request_id=2;request.payload=Encode({12,false,4});
    Send(fd,request);Send(fd,request);response=Receive(fd);assert(!response.status);
    request.operation=n::ControlMessage::PARAM_GET_REQUEST;request.request_id=3;request.payload.clear();Send(fd,request);response=Receive(fd);assert(response.request_id==3&&Decode(response.payload).count==12);
    Value local;std::uint64_t version=0;assert(owner.Get(local,&version)==0&&version==1);
    request.request_id=4;request.type.schema_id++;Send(fd,request);response=Receive(fd);assert(response.status&&response.reason==n::CompatibilityReason::TYPE_MISMATCH);
    auto malformed=std::vector<std::uint8_t>{};assert(n::EncodeControlFrame(request,malformed)==0);malformed[8]=255;
    const auto rejected=node.GetStats().rejected_frames;SendBytes(fd,malformed);Until([&]{return node.GetStats().rejected_frames>rejected;});close(fd);
    fd=Connect(server.port);Send(fd,hello);assert(Receive(fd).operation==n::ControlMessage::HELLO);
    request.type=Type();request.request_id=2;request.operation=n::ControlMessage::PARAM_SET_REQUEST;request.payload=Encode({77,true,7});Send(fd,request);
    request.request_id=5;request.operation=n::ControlMessage::PARAM_GET_REQUEST;request.payload.clear();Send(fd,request);response=Receive(fd);assert(response.request_id==5&&Decode(response.payload).count==12);
    assert(owner.Get(local,&version)==0&&version==1);close(fd);raw.Stop();node.Stop();
    assert(owner.Get(local)==0&&local.count==12);owner.Close();owner.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"partial TCP frames, inbound type gate, malformed close, duplicate Set/reconnect replay protection PASS\n";
}
void Correlation(){
    const auto port=Port();const std::string name="/mock";int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(listen(listener,4)==0);
    n::PeerDiscovery fake;auto config=Config(80,port).discovery;config.metadata_port=tcp;config.profile.capabilities=n::KNOWN_CAPABILITIES;
    auto endpoint=Fake(80,name);endpoint.application={Id(80),Id(42),99,123};endpoint.application_name="app";
    assert(fake.Start(config,{endpoint})==0);n::ControlNode node;assert(node.Start(Config(10,port))==0);
    std::thread server([&]{
        auto accept_peer=[&]{pollfd p{listener,POLLIN,0};assert(poll(&p,1,5000)>0);int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);assert(fd>=0);
            auto hello=Receive(fd);assert(hello.operation==n::ControlMessage::HELLO);
            hello.source=endpoint.identity.runtime;hello.session_id=fake.GetSessionId();Send(fd,hello,true);return fd;};
        auto reply=[&](int fd,n::ControlFrame f,int value){f.source=endpoint.identity.runtime;f.session_id=fake.GetSessionId();f.operation=n::ControlMessage::PARAM_GET_RESPONSE;f.timeout_ms=0;f.payload=Encode({value,true,1});Send(fd,f);};
        int fd=accept_peer();auto first=Receive(fd),second=Receive(fd),third=Receive(fd);
        assert(first.request_id<second.request_id&&second.request_id<third.request_id);
        reply(fd,third,300);reply(fd,second,200);reply(fd,third,300);
        std::this_thread::sleep_for(200ms);reply(fd,first,100);
        auto fourth=Receive(fd);reply(fd,fourth,400);std::this_thread::sleep_for(30ms);close(fd);
        fd=accept_peer();auto fifth=Receive(fd);reply(fd,fifth,500);(void)Receive(fd);close(fd);
    });
    Until([&]{return Peer(node,80).metadata_ready&&node.GetStats().connections>=1;});
    n::RemoteTarget application;application.scope=n::TargetScope::APPLICATION;application.application=endpoint.application;
    auto first=node.Get(Host(80),name,Type(),100ms);auto second=node.Get(application,name,Type(),2s);auto third=node.Get(Host(80),name,Type(),2s);
    auto r1=One(std::move(first)),r2=One(std::move(second)),r3=One(std::move(third));assert(r1.error==-ETIMEDOUT&&!r2.error&&!r3.error&&Decode(r2.value).count==200&&Decode(r3.value).count==300);
    Until([&]{return node.GetStats().responses_ignored>=2;});auto r4=One(node.Get(Host(80),name,Type(),2s));assert(!r4.error&&Decode(r4.value).count==400);
    Until([&]{return node.GetStats().connections>=2;});auto r5=One(node.Get(Host(80),name,Type(),2s));assert(!r5.error&&Decode(r5.value).count==500);
    auto dropped=One(node.Get(Host(80),name,Type(),2s));assert(dropped.error==-ECONNRESET);
    server.join();node.Stop();fake.Stop();close(listener);
    std::cout<<"APPLICATION target, out-of-order correlation, duplicate/late response, timeout and TCP reconnect PASS\n";
}
int main(int argc,char** argv){if(argc==3&&std::string(argv[1])=="--owner")return Owner(argv[2]);alarm(60);Codec();Basic(argv[0]);Failures();OwnerBinding(argv[0]);IncomingFrames();Correlation();}
