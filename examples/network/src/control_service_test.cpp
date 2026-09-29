#include "kcf/network/control_node.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/detail/supervisor_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
namespace n=kcf::network;
using namespace std::chrono_literals;
using R=n::CompatibilityReason;
n::HostIdentity Id(unsigned char x){n::HostIdentity id;id.bytes[0]=x;return id;}
auto Descriptor(){return kcf::TypeDescriptorTraits<Value>::Get();}
n::TypeIdentity Type(){n::TypeIdentity t;assert(!n::ParameterWireType(Descriptor(),t));return t;}
std::vector<std::uint8_t> Bytes(int count){Value v{count,true,1.5};kcf::DynamicPayload p;p.type_id=Type().type_id;p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> out;assert(!n::EncodeParameterValue(Descriptor(),p,out));return out;}
Value ValueOf(const std::vector<std::uint8_t>& bytes){kcf::DynamicPayload p;assert(!n::DecodeParameterValue(Descriptor(),bytes,p));Value v;std::memcpy(&v,p.bytes.data(),sizeof(v));return v;}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+8s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(5ms);}}
std::uint16_t Port(int kind=SOCK_DGRAM,int* keep=nullptr){int fd=socket(AF_INET,kind|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t len=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&len));if(keep)*keep=fd;else close(fd);return ntohs(a.sin_port);}
n::ControlConfig Config(unsigned char host,std::uint16_t port){n::ControlConfig c;c.discovery.host=Id(host);c.discovery.boot_id=Id(42);c.discovery.port=port;c.discovery.interface_address="127.0.0.1";c.discovery.announce_interval=50ms;c.discovery.peer_timeout=350ms;c.discovery.metadata_retry=30ms;c.metadata_interval=30ms;c.connect_timeout=300ms;c.reconnect_interval=50ms;c.metadata.runtime_pids={-1};return c;}
n::RemoteTarget Host(unsigned char host){n::RemoteTarget t;t.scope=n::TargetScope::HOST;t.host=Id(host);return t;}
n::RemoteTarget Endpoint(const n::NetworkEndpointMetadata& e){n::RemoteTarget t;t.endpoint=e.identity;return t;}
n::PeerInfo Peer(n::ControlNode& node,unsigned char host){for(const auto& p:node.GetPeers())if(p.host==Id(host))return p;return {};}
n::ServiceBatchResult Result(std::future<n::ServiceBatchResult> f){assert(f.wait_for(5s)==std::future_status::ready);return f.get();}
n::ServiceEndpointResult One(std::future<n::ServiceBatchResult> f){auto r=Result(std::move(f));assert(!r.error&&r.endpoints.size()==1);return r.endpoints[0];}
void Check(std::future<n::ServiceBatchResult> f,std::size_t count,int value){auto r=Result(std::move(f));assert(!r.error&&r.endpoints.size()==count);for(const auto& e:r.endpoints)assert(!e.error&&!e.execution_unknown&&ValueOf(e.value).count==value);}
volatile std::sig_atomic_t stopping=0,remove_service=0;
void Signal(int signal){if(signal==SIGUSR1)remove_service=1;else stopping=1;}
int Owner(std::uint16_t port,bool slow){alarm(90);std::signal(SIGTERM,Signal);std::signal(SIGUSR1,Signal);
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> parameter;const auto name="/kcf_service_shadow_"+std::to_string(getpid());assert(!parameter.Create(name,{1,true,1}));
    kcf::ServiceServer server;assert(!server.Create(port));
    assert((!server.Register<Value,Value>(1,"/calc",[&](const Value& request,Value& response){if(slow&&request.count==99)std::this_thread::sleep_for(500ms);response={request.count*2,!request.enabled,request.gain*2};})));
    assert((!server.Register<Value,Value>(2,"/local",[](const Value& request,Value& response){response=request;})));
    assert(!server.Start());kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    while(!stopping){if(remove_service){server.Stop();remove_service=0;}std::this_thread::sleep_for(5ms);}
    server.Stop();parameter.Close();parameter.Unlink();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);return 0;
}
void Codec(){n::ControlFrame f;f.operation=n::ControlMessage::SERVICE_REQUEST;f.source={Id(1),Id(42),123,1};f.session_id=1;f.request_id=2;f.endpoint={{Id(2),Id(42),321,1},1ull<<63};f.name="/calc";f.type=Type();f.response_type=Type();f.payload=Bytes(6);f.timeout_ms=100;
    std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(f,bytes)&&bytes.size()==196+5+13);n::ControlFrame out;assert(!n::DecodeControlFrame(bytes.data(),bytes.size(),out)&&out.response_type==f.response_type);
    for(std::size_t i=0;i<bytes.size();++i)assert(n::DecodeControlFrame(bytes.data(),i,out));
    for(auto offset:{0,5,7,11,15,179}){auto bad=bytes;bad[offset]=0xff;if(offset==179){for(unsigned j=172;j<196;++j)bad[j]=0;}assert(n::DecodeControlFrame(bad.data(),bad.size(),out));}
    f.operation=n::ControlMessage::SERVICE_RESPONSE;f.timeout_ms=0;f.payload=Bytes(12);assert(!n::EncodeControlFrame(f,bytes));assert(!n::DecodeControlFrame(bytes.data(),bytes.size(),out)&&ValueOf(out.payload).count==12);
    f.payload.pop_back();assert(n::EncodeControlFrame(f,bytes));f.payload.clear();f.status=-ETIMEDOUT;assert(!n::EncodeControlFrame(f,bytes));
    std::cout<<"Service operation extension/request+response type/framing malformed PASS\n";
}
void Integration(const char* exe){const auto port=Port();pid_t children[2];
    for(int i=0;i<2;++i){const auto local=std::to_string(Port());children[i]=fork();assert(children[i]>=0);if(!children[i]){execl(exe,exe,"--owner",local.c_str(),i?"slow":"fast",nullptr);_exit(127);}}
    kcf::detail::SupervisorRegistry supervisor;supervisor.Begin();kcf::SupervisorInfo app{};std::strcpy(app.application_name,"service_test");app.application_state=kcf::ApplicationState::RUNNING;app.element_count=2;
    for(int i=0;i<2;++i){auto& e=app.elements[i];e.pid=children[i];e.process_alive=1;std::strcpy(e.name,i?"slow":"fast");std::strcpy(e.executable,exe);Until([&]{return kcf::detail::ReadProcessIdentity(e.pid,e.process_start_ticks);});}supervisor.Update(app);
    n::ControlNode a,b;auto ca=Config(10,port),cb=Config(20,port);ca.metadata.runtime_pids={children[0],children[1]};ca.metadata.remote_services={"/calc"};ca.metadata.remote_parameters={"/kcf_service_shadow_"+std::to_string(children[0])};ca.metadata.groups={"robots"};assert(!a.Start(ca)&&!b.Start(cb));
    Until([&]{auto p=Peer(b,10);return p.metadata_ready&&p.endpoints.size()==6;});auto peer=Peer(b,10);
    assert(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_SERVICE));assert(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_ACTION));assert(!(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_TOPIC)));
    n::NetworkEndpointMetadata fast,slow;
    for(const auto& e:peer.endpoints){if(e.kind==n::EndpointKind::SERVICE){assert(e.identity.registration_id&(1ull<<63));assert(e.application_name=="service_test");if(e.name=="/calc"){if(e.identity.runtime.pid==children[0])fast=e;else slow=e;}}else assert(!(e.identity.registration_id&(1ull<<63)));}
    assert(n::Valid(fast)&&n::Valid(slow));
    auto call=[&](const n::RemoteTarget& t,int v=6,std::chrono::milliseconds timeout=2s){return b.CallService(t,"/calc",Type(),Type(),Bytes(v),timeout);};
    Check(b.Get(Host(10),ca.metadata.remote_parameters[0],Type(),2s),1,1);
    Check(call(Endpoint(fast)),1,12);Check(call(Host(10)),2,12);
    n::RemoteTarget app_target;app_target.scope=n::TargetScope::APPLICATION;app_target.application=fast.application;Check(call(app_target),2,12);
    n::RemoteTarget group;group.scope=n::TargetScope::GROUP;group.group="robots";Check(call(group),2,12);
    n::RemoteTarget all;all.scope=n::TargetScope::ALL;Check(call(all),2,12);
    std::vector<std::future<n::ServiceBatchResult>> calls;for(int i=1;i<=16;++i)calls.push_back(call(Endpoint(fast),i));for(int i=1;i<=16;++i)Check(std::move(calls[i-1]),1,i*2);
    assert(a.GetStats().connections==1&&b.GetStats().connections==1); // Parameter and Service share one TCP connection
    auto fanout=call(all,99,200ms);std::this_thread::sleep_for(30ms);auto independent=call(Endpoint(fast),4);assert(independent.wait_for(150ms)==std::future_status::ready);Check(std::move(independent),1,8);
    auto mixed=Result(std::move(fanout));assert(mixed.endpoints.size()==2);unsigned ok=0,timeouts=0;for(const auto& e:mixed.endpoints){if(!e.error)++ok;else{assert(e.error==-ETIMEDOUT&&e.execution_unknown);++timeouts;}}assert(ok==1&&timeouts==1);
    auto wrong=Type();wrong.type_id++;auto r=One(b.CallService(Endpoint(fast),"/calc",wrong,Type(),Bytes(2)));assert(r.reason==R::TYPE_MISMATCH&&!r.execution_unknown);
    r=One(b.CallService(Endpoint(fast),"/calc",Type(),wrong,Bytes(2)));assert(r.reason==R::TYPE_MISMATCH);
    auto local=Result(b.CallService(Host(10),"/local",Type(),Type(),Bytes(2)));assert(local.endpoints.size()==2);for(const auto& e:local.endpoints)assert(e.reason==R::LOCAL_ONLY);
    auto malformed=Bytes(2);malformed[4]=2;r=One(b.CallService(Endpoint(fast),"/calc",Type(),Type(),malformed));assert(r.error==-EPROTO);
    // Capture real local registration and prove removal while runtime remains alive.
    auto metadata=ca.metadata;metadata.host=ca.discovery.host;metadata.boot_id=ca.discovery.boot_id;
    std::vector<n::LocalEndpoint> entries;assert(!n::CollectLocalMetadata(metadata,entries));n::LocalEndpoint stale;
    for(const auto& e:entries)if(e.metadata.identity==slow.identity)stale=e;
    assert(!n::ValidateLocalServiceEndpoint(stale));assert(!kill(children[1],SIGUSR1));Until([&]{return n::ValidateLocalServiceEndpoint(stale)!=0;});
    auto gone=Result(call(Endpoint(slow)));assert(gone.error||gone.endpoints[0].error);
    assert(!kill(children[0],SIGKILL));int status;assert(waitpid(children[0],&status,0)==children[0]&&WIFSIGNALED(status));
    gone=Result(call(Endpoint(fast)));assert(gone.error||gone.endpoints[0].error);
    shm_unlink(("/kcf_service_shadow_"+std::to_string(children[0])).c_str());
    assert(!kill(children[1],SIGTERM));assert(waitpid(children[1],&status,0)==children[1]&&WIFEXITED(status));
    a.Stop();Until([&]{return Peer(b,10).state==n::PeerState::LOST;});assert(Result(call(Host(10))).error==-ENOTCONN);
    b.Stop();supervisor.End();std::cout<<"auto Service metadata/ID namespaces, all targets, parallel mixed results, gates, malformed payload, registration removal/process exit/LOST PASS\n";
}
void Gates(){auto port=Port();n::ControlNode client;assert(!client.Start(Config(10,port)));n::PeerDiscovery missing,incompatible;
    n::NetworkEndpointMetadata e;e.identity={{Id(30),Id(42),123,1},1ull<<63};e.kind=n::EndpointKind::SERVICE;e.name="/calc";e.element_name="test";e.scope=n::NetworkScope::REMOTE;e.request_type=Type();e.response_type=Type();
    auto c=Config(30,port).discovery;c.profile.capabilities=n::KNOWN_CAPABILITIES&~n::CapabilityBit(n::Capability::REMOTE_SERVICE);assert(!missing.Start(c,{e}));
    c.host=Id(40);c.profile.network.major=2;c.profile.capabilities=n::KNOWN_CAPABILITIES;e.identity.runtime.host=Id(40);assert(!incompatible.Start(c,{e}));
    Until([&]{return Peer(client,30).metadata_ready&&Peer(client,40).metadata_ready;});
    auto r=One(client.CallService(Host(30),"/calc",Type(),Type(),Bytes(2)));assert(r.reason==R::CAPABILITY_MISSING&&!r.execution_unknown);
    r=One(client.CallService(Host(40),"/calc",Type(),Type(),Bytes(2)));assert(r.reason==R::NETWORK_MAJOR_MISMATCH);
    assert(!client.GetStats().requests_sent);client.Stop();missing.Stop();incompatible.Stop();std::cout<<"missing Service capability and incompatible peer denied before send PASS\n";
}

void Send(int fd,const n::ControlFrame& f){std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(f,bytes));std::size_t offset=0;while(offset<bytes.size()){auto count=send(fd,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL);assert(count>0);offset+=count;}}
n::ControlFrame Receive(int fd){std::vector<std::uint8_t> bytes;std::size_t size=n::CONTROL_PREFIX_SIZE;while(bytes.size()<size){pollfd p{fd,POLLIN,0};assert(poll(&p,1,5000)>0);std::uint8_t data[2048];auto count=recv(fd,data,std::min(sizeof(data),size-bytes.size()),0);assert(count>0);bytes.insert(bytes.end(),data,data+count);if(bytes.size()==n::CONTROL_PREFIX_SIZE)assert(!n::ControlFrameSize(bytes.data(),bytes.size(),size));}n::ControlFrame f;assert(!n::DecodeControlFrame(bytes.data(),bytes.size(),f));return f;}
void Reconnect(){auto port=Port();int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(!listen(listener,4));
    n::PeerDiscovery server;n::ControlNode client;auto config=Config(80,port).discovery;config.metadata_port=tcp;config.profile.capabilities=n::KNOWN_CAPABILITIES;
    n::NetworkEndpointMetadata e;e.identity={{Id(80),Id(42),123,1},1ull<<63};e.kind=n::EndpointKind::SERVICE;e.name="/calc";e.element_name="mock";e.scope=n::NetworkScope::REMOTE;e.request_type=Type();e.response_type=Type();assert(!server.Start(config,{e}));assert(!client.Start(Config(10,port)));
    std::thread worker([&]{
        auto accept_peer=[&]{pollfd p{listener,POLLIN,0};assert(poll(&p,1,5000)>0);int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);assert(fd>=0);auto hello=Receive(fd);assert(hello.operation==n::ControlMessage::HELLO);hello.source=e.identity.runtime;hello.session_id=server.GetSessionId();Send(fd,hello);return fd;};
        auto response=[&](n::ControlFrame f){assert(f.operation==n::ControlMessage::SERVICE_REQUEST);f.source=e.identity.runtime;f.session_id=server.GetSessionId();f.operation=n::ControlMessage::SERVICE_RESPONSE;f.timeout_ms=0;f.payload=Bytes(ValueOf(f.payload).count*2);return f;};
        int fd=accept_peer();auto slow=Receive(fd),fast=Receive(fd);auto ready=response(fast);Send(fd,ready);Send(fd,ready);std::this_thread::sleep_for(180ms);Send(fd,response(slow));
        auto next=response(Receive(fd));std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(next,bytes));assert(send(fd,bytes.data(),20,MSG_NOSIGNAL)==20);close(fd); // response interrupted after prefix
        fd=accept_peer();Send(fd,response(Receive(fd)));std::this_thread::sleep_for(20ms);close(fd);
    });
    Until([&]{return Peer(client,80).metadata_ready&&client.GetStats().connections==1;});
    auto slow=client.CallService(Host(80),"/calc",Type(),Type(),Bytes(4),80ms);
    auto fast=client.CallService(Host(80),"/calc",Type(),Type(),Bytes(7),2s);
    Check(std::move(fast),1,14);auto timeout=One(std::move(slow));assert(timeout.error==-ETIMEDOUT&&timeout.execution_unknown);
    Until([&]{return client.GetStats().responses_ignored>=2;});
    auto interrupted=One(client.CallService(Host(80),"/calc",Type(),Type(),Bytes(9),2s));assert(interrupted.error==-ECONNRESET&&interrupted.execution_unknown);
    Until([&]{return client.GetStats().connections>=2;});Check(client.CallService(Host(80),"/calc",Type(),Type(),Bytes(11),2s),1,22);
    worker.join();client.Stop();server.Stop();close(listener);
    std::cout<<"Service late/duplicate responses, interrupted response unknown execution, reconnect with new request PASS\n";
}
int main(int argc,char** argv){if(argc==4&&std::string(argv[1])=="--owner")return Owner(std::stoi(argv[2]),std::string(argv[3])=="slow");alarm(60);Codec();Integration(argv[0]);Gates();Reconnect();}
