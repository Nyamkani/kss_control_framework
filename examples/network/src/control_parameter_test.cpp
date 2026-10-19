#include "kcf/network/control_node.hpp"
#include "kcf/network/network_diagnostics.hpp"
#include "kcf/network/network_diagnostics_client.hpp"
#include "kcf/ipc/publisher.hpp"
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
    // The general Control replay window is shared by mutating operations and
    // survives reconnect. Stale Service/Action requests must not dispatch or
    // generate an extra response ahead of the fresh Get below.
    for(auto operation:{n::ControlMessage::SERVICE_REQUEST,n::ControlMessage::ACTION_GOAL_REQUEST}) {
        auto duplicate=request;duplicate.operation=operation;duplicate.response_type=Type();
        if(operation==n::ControlMessage::ACTION_GOAL_REQUEST){duplicate.feedback_type=Type();duplicate.goal={duplicate.source,1,duplicate.session_id};}
        Send(fd,duplicate);
    }
    request.request_id=5;request.operation=n::ControlMessage::PARAM_GET_REQUEST;request.payload.clear();Send(fd,request);response=Receive(fd);assert(response.request_id==5&&Decode(response.payload).count==12);
    assert(owner.Get(local,&version)==0&&version==1);close(fd);raw.Stop();node.Stop();
    assert(owner.Get(local)==0&&local.count==12);owner.Close();owner.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"partial TCP frames, inbound type gate, malformed close, duplicate Set/Service/Action reconnect replay protection PASS\n";
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

n::PeerInfo Observed(n::PeerDiscovery& observer,unsigned char host) {
    for(const auto& peer:observer.GetPeers())if(peer.host==Id(host))return peer;
    return {};
}
n::ControlFrame ObserverHello(n::PeerDiscovery& observer,unsigned char host) {
    n::ControlFrame hello;hello.source={Id(host),Id(42),getpid(),1};hello.session_id=observer.GetSessionId();return hello;
}
void Closed(int fd) {
    Until([&]{std::uint8_t bytes[4096];auto count=recv(fd,bytes,sizeof(bytes),MSG_DONTWAIT);
        return count==0 || (count<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR);});
    close(fd);
}
void Diagnostics() {
    const auto port=Port();auto config=Config(100,port);
    n::ControlNode gateway;n::PeerDiscovery low,high;
    auto low_config=Config(1,port).discovery,high_config=Config(200,port).discovery;
    assert(!low.Start(low_config) && !high.Start(high_config));
    // User-supplied bits do not advertise an opt-out handler.
    config.discovery.profile.capabilities=n::KNOWN_CAPABILITIES;
    assert(!config.diagnostics_enabled && !gateway.Start(config));
    Until([&]{return Peer(gateway,1).state==n::PeerState::ONLINE && Observed(low,100).session_id;});
    assert(!(Observed(low,100).profile.capabilities&n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS)));
    int fd=Connect(Observed(low,100).port);Send(fd,ObserverHello(low,1));Closed(fd);
    gateway.Stop();config.diagnostics_enabled=true;assert(!gateway.Start(config));
    const auto open=[&](n::PeerDiscovery& observer,unsigned char host) {
        Until([&]{auto p=Observed(observer,100);return p.state==n::PeerState::ONLINE &&
            (p.profile.capabilities&n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS)) && Peer(gateway,host).session_id==observer.GetSessionId();});
        int socket=Connect(Observed(observer,100).port);Send(socket,ObserverHello(observer,host));
        auto hello=Receive(socket);assert(hello.operation==n::ControlMessage::HELLO && hello.source.host==Id(100));return socket;
    };
    const auto request=[&](n::PeerDiscovery& observer,unsigned char host,std::uint64_t id,unsigned timeout=1000) {
        auto f=ObserverHello(observer,host);f.operation=n::ControlMessage::DIAGNOSTICS_REQUEST;
        f.target=n::TargetScope::HOST;f.request_id=id;f.timeout_ms=timeout;return f;
    };
    const auto call=[&](int socket,const n::ControlFrame& f) {
        Send(socket,f);auto response=Receive(socket);
        assert(response.operation==n::ControlMessage::DIAGNOSTICS_RESPONSE && response.request_id==f.request_id);
        assert(response.source.host==Id(100) && response.source.boot_id==Id(42) && !response.timeout_ms);
        if(response.status)assert(response.status<0 && response.payload.empty());
        else {n::NetworkDiagnosticsSnapshot snapshot;
            assert(!n::DecodeNetworkDiagnosticsSnapshot(response.payload.data(),response.payload.size(),snapshot));
            assert(snapshot.gateway_host==response.source.host && snapshot.gateway_boot_id==response.source.boot_id && snapshot.gateway_session==response.session_id);
            assert(response.payload.size()<=n::MAX_NETWORK_DIAGNOSTICS_PAYLOAD);
        }
        return response;
    };
    fd=open(low,1);int upper=open(high,200); // both Host ID directions, no outbound gateway connection
    auto response=call(fd,request(low,1,1));assert(!response.status);
    n::NetworkDiagnosticsSnapshot snapshot;assert(!n::DecodeNetworkDiagnosticsSnapshot(response.payload.data(),response.payload.size(),snapshot));
    assert(snapshot.routes.empty() && snapshot.total_routes==0 && !snapshot.truncated);
    assert(call(fd,request(low,1,1)).status==-EALREADY);
    assert(call(fd,request(low,1,2)).status==-EAGAIN);
    assert(!call(upper,request(high,200,1)).status);close(upper);
    std::uint64_t id=2;bool expired=false;
    Until([&]{const auto next=std::chrono::steady_clock::now()+110ms;Until([&]{return std::chrono::steady_clock::now()>=next;});
        const auto r=call(fd,request(low,1,++id,1));expired=r.status==-ETIMEDOUT;return expired;});
    close(fd);Until([&]{return gateway.GetStats().connections>=2;});
    // Reconnect does not reset the high-water mark for this Host/boot/session.
    // Retry HELLO only after the old fd has been consumed by the IO loop.
    const auto settle=std::chrono::steady_clock::now()+50ms;Until([&]{return std::chrono::steady_clock::now()>=settle;});
    fd=open(low,1);assert(call(fd,request(low,1,id)).status==-EALREADY);
    auto wrong=request(low,1,++id);++wrong.session_id;Send(fd,wrong);Closed(fd);
    // The observer is never allowed to use Parameter, Service, or Action handlers.
    for(auto operation:{n::ControlMessage::PARAM_GET_REQUEST,n::ControlMessage::SERVICE_REQUEST,n::ControlMessage::ACTION_GOAL_REQUEST}) {
        fd=open(low,1);auto f=request(low,1,++id);f.operation=operation;f.target=n::TargetScope::ENDPOINT;
        f.endpoint={{Id(100),Id(42),getpid(),1},1};f.name="/forbidden";f.type=Type();
        if(operation!=n::ControlMessage::PARAM_GET_REQUEST){f.response_type=Type();f.payload=Encode({1,true,1});}
        if(operation==n::ControlMessage::ACTION_GOAL_REQUEST){f.feedback_type=Type();f.goal={f.source,1,f.session_id};}
        Send(fd,f);Closed(fd);
    }
    fd=open(low,1);auto malformed=request(low,1,++id);std::vector<std::uint8_t> wire;
    assert(!n::EncodeControlFrame(malformed,wire));std::fill(wire.begin()+156,wire.begin()+160,0);SendBytes(fd,wire);Closed(fd);
    // A coalesced flood saturates the bounded response queue and is disconnected.
    fd=open(low,1);std::vector<std::uint8_t> flood;
    for(int i=0;i<64;++i){assert(!n::EncodeControlFrame(request(low,1,++id),wire));flood.insert(flood.end(),wire.begin(),wire.end());}
    assert(send(fd,flood.data(),flood.size(),MSG_NOSIGNAL)>0);Closed(fd);
    fd=open(low,1);low.Stop();Until([&]{return Peer(gateway,1).state==n::PeerState::LOST;});Closed(fd);
    assert(!low.Start(low_config));fd=open(low,1);assert(!call(fd,request(low,1,1)).status);close(fd);
    // Incompatible profile and stale boot/session cannot pass HELLO.
    n::PeerDiscovery incompatible;auto incompatible_config=Config(250,port).discovery;
    incompatible_config.profile.network.major=2;assert(!incompatible.Start(incompatible_config));
    Until([&]{return Peer(gateway,250).session_id!=0;});fd=Connect(Observed(low,100).port);Send(fd,ObserverHello(incompatible,250));Closed(fd);incompatible.Stop();
    fd=Connect(Observed(low,100).port);auto stale=ObserverHello(low,1);stale.source.boot_id=Id(88);Send(fd,stale);Closed(fd);
    // Eight independent observers fit; the ninth is refused without taking down the gateway.
    std::vector<std::unique_ptr<n::PeerDiscovery>> observers;std::vector<int> sockets;
    const auto drained=std::chrono::steady_clock::now()+50ms;Until([&]{return std::chrono::steady_clock::now()>=drained;});
    for(unsigned char host=10;host<19;++host){auto peer=std::make_unique<n::PeerDiscovery>();assert(!peer->Start(Config(host,port).discovery));
        Until([&]{return Peer(gateway,host).session_id==peer->GetSessionId();});
        if(host<18)sockets.push_back(open(*peer,host));
        else {fd=Connect(Observed(low,100).port);Send(fd,ObserverHello(*peer,host));Closed(fd);}
        observers.push_back(std::move(peer));
    }
    for(int socket:sockets)close(socket);
    for(auto& peer:observers)peer->Stop();
    assert(!gateway.GetLastError());gateway.Stop();
    // Failed listener startup must not advertise diagnostics.
    int occupied=-1;auto broken=config;broken.discovery.host=Id(251);broken.discovery.metadata_port=Port(SOCK_STREAM,&occupied);
    assert(gateway.Start(broken)<0);assert(Observed(low,251).session_id==0);close(occupied);
    // Real Topic sender route: one publish produces a stable, nonzero counter.
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    const auto name="/diagnostics_topic_"+std::to_string(getpid());kcf::Publisher<Value> publisher;
    assert(!publisher.Create(name));assert(!publisher.Publish({7,true,1}));
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    config.topics.enabled=true;config.topics.port=Port();config.metadata.runtime_pids={getpid()};config.metadata.remote_topics={name};
    n::PeerDiscovery consumer;auto consumer_config=Config(60,port).discovery;consumer_config.interface_address="127.0.0.60";
    consumer_config.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::REMOTE_TOPIC)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
    auto endpoint=Fake(60,name);endpoint.kind=n::EndpointKind::TOPIC;endpoint.role=n::EndpointRole::CONSUMER;
    assert(!consumer.Start(consumer_config,{endpoint}));assert(!gateway.Start(config));
    Until([&]{auto p=Observed(low,100);return p.metadata_ready && !p.endpoints.empty() && Peer(gateway,60).metadata_ready;});
    auto peer=Observed(low,100);n::TopicPacket interest;interest.kind=n::TopicMessage::INTEREST;
    interest.source=peer.endpoints[0].identity;interest.destination=endpoint.identity;
    interest.source_session=peer.session_id;interest.destination_session=consumer.GetSessionId();interest.route_id=1;interest.name=name;interest.type=Type();
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(udp>=0);sockaddr_in destination{};destination.sin_family=AF_INET;destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);destination.sin_port=htons(config.topics.port);
    sockaddr_in sender=destination;assert(inet_pton(AF_INET,"127.0.0.60",&sender.sin_addr)==1);
    assert(!bind(udp,reinterpret_cast<sockaddr*>(&sender),sizeof(sender)));
    assert(!n::EncodeTopicPacket(interest,wire));assert(sendto(udp,wire.data(),wire.size(),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==static_cast<ssize_t>(wire.size()));
    Until([&]{auto d=gateway.GetTopicDiagnostics();return !d.routes.empty() && d.routes[0].packets_sent>0;});
    const auto original=gateway.GetTopicDiagnostics();fd=open(low,1);response=call(fd,request(low,1,1));assert(!response.status);
    assert(!n::DecodeNetworkDiagnosticsSnapshot(response.payload.data(),response.payload.size(),snapshot));
    assert(snapshot.total_routes==original.routes.size() && snapshot.routes.size()==original.routes.size());
    const auto& a=snapshot.routes[0];const auto& b=original.routes[0];
    assert(a.source==b.source && a.destination==b.destination && a.route_id==b.route_id && a.name==b.name);
    assert(a.source_session==b.source_session && a.destination_session==b.destination_session && a.sending==b.sending);
    assert(a.packets_sent==b.packets_sent && a.packets_sent>0 && a.packets_received==b.packets_received && a.samples_published==b.samples_published);
    assert(a.dropped==b.dropped && a.duplicate==b.duplicate && a.out_of_order==b.out_of_order && a.missed_sequence==b.missed_sequence && a.malformed==b.malformed && a.local_samples_skipped==b.local_samples_skipped);
    Until([&]{return Observed(high,100).session_id==peer.session_id &&
        Peer(gateway,200).session_id==high.GetSessionId();});
    n::NetworkDiagnosticsClient route_client(high,high_config);n::NetworkDiagnosticsSnapshot public_snapshot;
    assert(!route_client.Query(Id(100),public_snapshot));
    assert(public_snapshot.routes.size()==snapshot.routes.size() && public_snapshot.routes[0].packets_sent==a.packets_sent);
    std::vector<std::uint8_t> public_bytes,raw_bytes;
    assert(!n::EncodeNetworkDiagnosticsSnapshot(public_snapshot,public_bytes));
    assert(!n::EncodeNetworkDiagnosticsSnapshot(snapshot,raw_bytes));assert(public_bytes==raw_bytes);
    close(fd);gateway.Stop();close(udp);consumer.Stop();config.topics.enabled=false;assert(!gateway.Start(config));
    assert(gateway.GetTopicDiagnostics().routes.empty());
    Until([&]{return Observed(low,100).session_id!=peer.session_id;});fd=open(low,1);response=call(fd,request(low,1,1));assert(!response.status);
    assert(!n::DecodeNetworkDiagnosticsSnapshot(response.payload.data(),response.payload.size(),snapshot) && snapshot.routes.empty());close(fd);
    gateway.Stop();low.Stop();high.Stop();publisher.Close();publisher.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Diagnostics opt-in, observer HELLO/directions, gates, replay, timeout, bounds, real route counters and disabled restart PASS\n";
}

void DiagnosticsReplayLifecycle() {
    const auto port=Port();auto config=Config(240,port);config.diagnostics_enabled=true;
    config.discovery.announce_interval=20ms;config.discovery.peer_timeout=80ms;
    n::ControlNode gateway;assert(!gateway.Start(config));
    const auto observer_config=[&](unsigned char host){auto c=config.discovery;c.host=Id(host);return c;};
    n::PeerDiscovery active,online;assert(!active.Start(observer_config(1)) && !online.Start(observer_config(2)));
    Until([&]{return Observed(active,240).session_id && Peer(gateway,1).session_id && Peer(gateway,2).session_id;});
    const auto tcp=Observed(active,240).port;
    const auto open=[&](const n::ControlFrame& hello){int fd=Connect(tcp);Send(fd,hello);assert(Receive(fd).operation==n::ControlMessage::HELLO);return fd;};
    const auto query=[&](int fd,const n::ControlFrame& hello,std::uint64_t id){auto f=hello;
        f.operation=n::ControlMessage::DIAGNOSTICS_REQUEST;f.target=n::TargetScope::HOST;f.request_id=id;f.timeout_ms=1000;
        Send(fd,f);auto response=Receive(fd);assert(response.request_id==id && response.operation==n::ControlMessage::DIAGNOSTICS_RESPONSE);return response.status;};
    auto active_hello=ObserverHello(active,1),online_hello=ObserverHello(online,2);
    int held=open(active_hello);assert(!query(held,active_hello,9));
    int fd=open(online_hello);assert(!query(fd,online_hello,9));close(fd);
    n::ControlFrame last_hello;n::PeerInfo last_peer;
    for(unsigned char host=10;host<80;++host) {
        n::PeerDiscovery observer;assert(!observer.Start(observer_config(host)));
        Until([&]{return Peer(gateway,host).session_id==observer.GetSessionId();});
        auto hello=ObserverHello(observer,host);fd=open(hello);assert(!query(fd,hello,7));
        if(host==79){last_hello=hello;last_peer=Peer(gateway,host);}
        close(fd);observer.Stop();Until([&]{return Peer(gateway,host).state==n::PeerState::LOST;});
    }
    // Both a live connection and an ONLINE observer without TCP survive churn.
    assert(query(held,active_hello,9)==-EALREADY);
    fd=open(online_hello);assert(query(fd,online_hello,9)==-EALREADY);close(fd);
    close(held);std::this_thread::sleep_for(30ms);
    held=open(active_hello);assert(query(held,active_hello,9)==-EALREADY);close(held);
    // Explicitly resurrect the last LOST incarnation, without changing session.
    // After reclamation its read-only request IDs belong to a new replay window.
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(udp>=0);
    in_addr loopback{};loopback.s_addr=htonl(INADDR_LOOPBACK);
    assert(!setsockopt(udp,IPPROTO_IP,IP_MULTICAST_IF,&loopback,sizeof(loopback)));
    sockaddr_in group{};group.sin_family=AF_INET;group.sin_port=htons(port);
    assert(inet_pton(AF_INET,config.discovery.multicast_group.c_str(),&group.sin_addr)==1);
    n::DiscoveryPacket announce;announce.host=last_hello.source.host;announce.boot_id=last_hello.source.boot_id;
    announce.session_id=last_hello.session_id;announce.sequence=1000000;announce.revision=last_peer.metadata_revision;
    std::vector<std::uint8_t> metadata,wire;assert(!n::EncodeDiscoveryMetadata({},metadata));announce.metadata_size=metadata.size();
    assert(!n::EncodeDiscoveryPacket(announce,wire));
    assert(sendto(udp,wire.data(),wire.size(),0,reinterpret_cast<sockaddr*>(&group),sizeof(group))==static_cast<ssize_t>(wire.size()));
    Until([&]{return Peer(gateway,79).state==n::PeerState::ONLINE;});
    fd=open(last_hello);assert(!query(fd,last_hello,7));assert(query(fd,last_hello,7)==-EALREADY);close(fd);close(udp);
    active.Stop();online.Stop();gateway.Stop();
    std::cout<<"Diagnostics replay lifecycle: 70 retired Hosts, ONLINE/connected protection, reconnect replay, LOST reclamation and same-session new window PASS\n";
}

void DiagnosticsClient() {
    const auto port=Port();auto cfg=Config(100,port);cfg.diagnostics_enabled=true;
    n::ControlNode gateway;n::PeerDiscovery low,high;
    auto lc=Config(1,port).discovery,hc=Config(200,port).discovery;
    assert(!low.Start(lc) && !high.Start(hc) && !gateway.Start(cfg));
    n::NetworkDiagnosticsClient client(low,lc),upper(high,hc);n::NetworkDiagnosticsSnapshot result;
    const auto ready=[&]{Until([&]{return Observed(low,100).session_id && Observed(high,100).session_id &&
        Peer(gateway,1).session_id==low.GetSessionId() && Peer(gateway,200).session_id==high.GetSessionId();});};
    ready();assert(client.Query(Id(100),result,0ms)==-EINVAL);
    assert(client.Query(Id(99),result)==-ENOENT);
    assert(!client.Query(Id(100),result) && result.gateway_host==Id(100));
    assert(!upper.Query(Id(100),result));
    // Each call reconnects; IDs also survive constructing a replacement client.
    std::this_thread::sleep_for(120ms);assert(!client.Query(Id(100),result));
    std::this_thread::sleep_for(120ms);n::NetworkDiagnosticsClient replacement(low,lc);
    assert(!replacement.Query(Id(100),result));const auto old_session=result.gateway_session;
    gateway.Stop();Until([&]{return Observed(low,100).state==n::PeerState::LOST;});
    assert(client.Query(Id(100),result)==-ENOTCONN && result.gateway_session==old_session);
    assert(!gateway.Start(cfg));Until([&]{auto p=Observed(low,100);return p.session_id && p.session_id!=old_session;});ready();
    assert(!client.Query(Id(100),result) && result.gateway_session!=old_session);
    gateway.Stop();cfg.diagnostics_enabled=false;const auto session=result.gateway_session;
    assert(!gateway.Start(cfg));Until([&]{auto p=Observed(low,100);return p.session_id && p.session_id!=session;});
    assert(client.Query(Id(100),result)==-ENOTSUP);
    gateway.Stop();low.Stop();high.Stop();
    std::cout<<"Public Diagnostics client: actual gateways, both Host directions, repeated/replacement clients, LOST/restart and capability gate PASS\n";
}
volatile std::sig_atomic_t diagnostics_signals=0;
void DiagnosticsSignal(int){++diagnostics_signals;}
void DiagnosticsClientWire() {
    struct sigaction action{},previous{};action.sa_handler=DiagnosticsSignal;sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR2,&action,&previous));const auto caller=pthread_self();
    const auto fd_count=[] {return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});};
    enum Mode {GOOD,WRONG_ID,WRONG_HELLO,WRONG_SESSION,WRONG_RUNTIME,BAD_SNAPSHOT,BAD_FRAME,UNEXPECTED,
        OVERSIZE,REMOTE_ERROR,DISCONNECT,HELLO_TIMEOUT,RESPONSE_TIMEOUT,TRUNCATED,SESSION_CHANGE,INCOMPATIBLE};
    const auto port=Port();auto oc=Config(2,port).discovery;n::PeerDiscovery observer;assert(!observer.Start(oc));
    n::NetworkDiagnosticsClient client(observer,oc);std::uint64_t previous_id=0;
    for(auto mode:{GOOD,WRONG_ID,WRONG_HELLO,WRONG_SESSION,WRONG_RUNTIME,BAD_SNAPSHOT,BAD_FRAME,UNEXPECTED,
                  OVERSIZE,REMOTE_ERROR,DISCONNECT,HELLO_TIMEOUT,RESPONSE_TIMEOUT,TRUNCATED,SESSION_CHANGE,INCOMPATIBLE}) {
        int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(!listen(listener,4));
        n::PeerDiscovery fake;auto fc=Config(80,port).discovery;fc.metadata_port=tcp;
        fc.profile.capabilities|=n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS);
        if(mode==INCOMPATIBLE)fc.profile.network.major=2;
        assert(!fake.Start(fc));Until([&]{return Observed(observer,80).session_id==fake.GetSessionId();});
        n::NetworkDiagnosticsSnapshot expected;expected.gateway_host=Id(80);expected.gateway_boot_id=Id(42);expected.gateway_session=fake.GetSessionId();
        n::TopicRouteInfo route;route.source={{Id(80),Id(42),getpid(),1},1};route.destination={{Id(2),Id(42),getpid(),1},2};
        route.source_session=3;route.destination_session=4;route.route_id=5;route.name="/"+std::string(239,'q');
        route.state=n::TopicRouteState::ACTIVE;route.sending=true;route.error=-EIO;route.reason=n::CompatibilityReason::TYPE_MISMATCH;
        route.packets_sent=11;route.packets_received=12;route.samples_published=13;route.dropped=14;route.duplicate=15;
        route.out_of_order=16;route.missed_sequence=17;route.malformed=18;route.local_samples_skipped=19;
        expected.routes.assign(100,route);expected.total_routes=100;
        std::vector<std::uint8_t> payload;assert(!n::EncodeNetworkDiagnosticsSnapshot(expected,payload));
        assert(!n::DecodeNetworkDiagnosticsSnapshot(payload.data(),payload.size(),expected) && expected.truncated);
        const auto descriptors=fd_count();std::thread server;
        if(mode!=INCOMPATIBLE)server=std::thread([&]{
            pollfd poller{listener,POLLIN,0};assert(poll(&poller,1,3000)>0);int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);assert(fd>=0);
            auto hello=Receive(fd);assert(hello.source.host==oc.host && hello.source.boot_id==oc.boot_id && hello.session_id==observer.GetSessionId());
            if(mode==HELLO_TIMEOUT){std::this_thread::sleep_for(160ms);close(fd);return;}
            hello.source={Id(80),Id(42),getpid(),1};hello.session_id=fake.GetSessionId();
            if(mode==WRONG_HELLO){++hello.session_id;Send(fd,hello);close(fd);return;}
            if(mode==GOOD){std::this_thread::sleep_for(10ms);assert(!pthread_kill(caller,SIGUSR2));}
            Send(fd,hello,mode==GOOD);auto request=Receive(fd);
            assert(request.operation==n::ControlMessage::DIAGNOSTICS_REQUEST && request.target==n::TargetScope::HOST);
            assert(request.request_id>previous_id);previous_id=request.request_id;
            if(mode==DISCONNECT){close(fd);return;}
            if(mode==RESPONSE_TIMEOUT){std::this_thread::sleep_for(160ms);close(fd);return;}
            n::ControlFrame response=request;response.operation=n::ControlMessage::DIAGNOSTICS_RESPONSE;
            response.source=hello.source;response.session_id=hello.session_id;response.timeout_ms=0;response.payload=payload;
            if(mode==WRONG_ID)++response.request_id;
            if(mode==WRONG_SESSION){++response.session_id;auto changed=expected;changed.gateway_session=response.session_id;
                assert(!n::EncodeNetworkDiagnosticsSnapshot(changed,response.payload));}
            if(mode==WRONG_RUNTIME)++response.source.process_start_ticks;
            if(mode==REMOTE_ERROR){response.status=-EAGAIN;response.payload.clear();}
            if(mode==UNEXPECTED){Send(fd,hello);close(fd);return;}
            if(mode==SESSION_CHANGE){fake.Stop();assert(!fake.Start(fc));
                Until([&]{return Observed(observer,80).session_id==fake.GetSessionId();});}
            std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(response,bytes));
            if(mode==BAD_SNAPSHOT)bytes[172+12]^=1;
            if(mode==BAD_FRAME)bytes[0]^=1;
            if(mode==OVERSIZE){bytes[8]=0;bytes[9]=0;bytes[10]=0x41;bytes[11]=0;bytes.resize(n::CONTROL_PREFIX_SIZE);}
            if(mode==TRUNCATED)bytes.resize(177);
            if(mode==GOOD){for(std::size_t offset=0;offset<bytes.size();offset+=31){
                const auto count=std::min<std::size_t>(31,bytes.size()-offset);
                assert(send(fd,bytes.data()+offset,count,MSG_NOSIGNAL)==static_cast<ssize_t>(count));std::this_thread::sleep_for(100us);}}
            else {assert(send(fd,bytes.data(),bytes.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(bytes.size()));}
            close(fd);
        });
        n::NetworkDiagnosticsSnapshot result;result.gateway_session=999;result.total_routes=123;
        const bool timeout=mode==HELLO_TIMEOUT||mode==RESPONSE_TIMEOUT;
        const auto start=std::chrono::steady_clock::now();const int code=client.Query(Id(80),result,timeout?100ms:1500ms);
        const auto elapsed=std::chrono::steady_clock::now()-start;
        if(server.joinable())server.join();
        assert(fd_count()==descriptors);
        if(mode==GOOD){assert(!code);std::vector<std::uint8_t> actual;assert(!n::EncodeNetworkDiagnosticsSnapshot(result,actual));assert(actual==payload);}
        else {assert(code<0 && result.gateway_session==999 && result.total_routes==123);
            if(timeout)assert(code==-ETIMEDOUT && elapsed<300ms);
            if(mode==WRONG_ID||mode==BAD_FRAME||mode==UNEXPECTED||mode==BAD_SNAPSHOT)assert(code==-EPROTO);
            if(mode==WRONG_HELLO||mode==WRONG_SESSION||mode==WRONG_RUNTIME||mode==SESSION_CHANGE)assert(code==-ESTALE);
            if(mode==OVERSIZE)assert(code==-EMSGSIZE);
            if(mode==REMOTE_ERROR)assert(code==-EAGAIN);
            if(mode==DISCONNECT||mode==TRUNCATED)assert(code==-ECONNRESET);
            if(mode==INCOMPATIBLE)assert(code==-EPROTONOSUPPORT);
        }
        fake.Stop();close(listener);
    }
    assert(diagnostics_signals>0);assert(!sigaction(SIGUSR2,&previous,nullptr));
    observer.Stop();std::cout<<"Public Diagnostics client: partial TCP, EINTR, socket cleanup, deadline/EOF, correlation/identity/stale/malformed gates, bounded truncated snapshot and all counters PASS\n";
}

int main(int argc,char** argv){if(argc==3&&std::string(argv[1])=="--owner")return Owner(argv[2]);alarm(60);Codec();Diagnostics();DiagnosticsReplayLifecycle();DiagnosticsClient();DiagnosticsClientWire();Basic(argv[0]);Failures();OwnerBinding(argv[0]);IncomingFrames();Correlation();}
