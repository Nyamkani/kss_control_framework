#include "kcf/network/remote_control_client.hpp"
#include "kcf/network/network_diagnostics_client.hpp"
#include "kcf/network/control_node.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/action/action_server.hpp"
#include <array>
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include "../../../kcf/src/network/control_client_io.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
struct DescriptorArray { bool enabled; std::int32_t samples[3]; double gain; };
namespace kcf { template<> struct TypeDescriptorTraits<DescriptorArray> {
    static constexpr bool defined=true;
    static TypeDescriptor Get(){auto d=MakeTypeDescriptor<DescriptorArray>("test.descriptor.array.v1");d.field_count=3;
        d.fields[0]=MakeField<DescriptorArray,bool>("enabled",offsetof(DescriptorArray,enabled));
        d.fields[1]=MakeField<DescriptorArray,std::int32_t[3]>("samples",offsetof(DescriptorArray,samples));
        d.fields[2]=MakeField<DescriptorArray,double>("gain",offsetof(DescriptorArray,gain));return d;}
}; }
using namespace std::chrono_literals;
namespace n=kcf::network;
n::HostIdentity Id(unsigned char x){n::HostIdentity h;h.bytes[0]=x;return h;}
template<class F>void Until(F f){auto end=std::chrono::steady_clock::now()+5s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
std::uint16_t Port(int type=SOCK_DGRAM,int* keep=nullptr){int fd=socket(AF_INET,type|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t l=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&l));if(keep)*keep=fd;else close(fd);return ntohs(a.sin_port);}
n::DiscoveryConfig Config(unsigned char h,std::uint16_t port){n::DiscoveryConfig c;c.host=Id(h);c.boot_id=Id(80);c.interface_address="127.0.0.1";c.port=port;c.announce_interval=50ms;c.peer_timeout=300ms;return c;}
// Bypass client preflight to prove Gateway operation/type gates on the wire.
int Raw(n::PeerDiscovery& discovery,const n::DiscoveryConfig& dc,const n::PeerInfo& peer,
        const n::NetworkEndpointMetadata& e,n::ControlMessage operation,bool wrong_type=false,bool host_scope=false) {
    using namespace n::client_io;
    std::this_thread::sleep_for(100ms);
    Socket socket;socket.fd=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(socket.fd>=0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(peer.port);inet_pton(AF_INET,peer.address.c_str(),&a.sin_addr);
    int code=connect(socket.fd,reinterpret_cast<sockaddr*>(&a),sizeof(a));assert(!code||errno==EINPROGRESS);
    auto end=Clock::now()+2s;assert(!Wait(socket.fd,POLLOUT,end));
    n::ControlFrame f;f.source={dc.host,dc.boot_id,getpid(),0};assert(kcf::detail::ReadProcessIdentity(getpid(),f.source.process_start_ticks));f.session_id=discovery.GetSessionId();
    assert(!SendFrame(socket.fd,f,end));n::ControlFrame hello;assert(!ReceiveFrame(socket.fd,n::ControlMessage::HELLO,hello,end));
    f.operation=operation;f.endpoint=e.identity;f.name=e.name;f.type=e.value_type;f.timeout_ms=1000;f.request_id=100000;
    if(wrong_type)++f.type.type_id;
    if(host_scope)f.target=n::TargetScope::HOST;
    if(operation==n::ControlMessage::PARAM_SET_REQUEST)f.payload.resize(e.value_type.payload_size);
    assert(!SendFrame(socket.fd,f,end));n::ControlFrame response;
    code=ReceiveFrame(socket.fd,operation==n::ControlMessage::PARAM_GET_REQUEST?n::ControlMessage::PARAM_GET_RESPONSE:n::ControlMessage::PARAM_SET_RESPONSE,response,end);
    return code?code:response.status;
}
void ServiceClientTests() {
    auto port=Port();auto dc=Config(51,port);n::PeerDiscovery discovery;assert(!discovery.Start(dc));
    n::RemoteControlClient client(discovery,dc);assert(!client.Start());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    std::atomic<int> calls{0};kcf::ServiceServer service;assert(!service.Create(Port()));
    assert((!service.Register<Value,Value>(1,"/rcc/service",[&](const Value& in,Value& out){
        ++calls;if(in.count==99)std::this_thread::sleep_for(350ms);out=in;out.count*=2;
    })));
    assert(!service.Start());kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlConfig gc;gc.discovery=Config(50,port);gc.metadata.runtime_pids={getpid()};gc.metadata.remote_services={"/rcc/service"};
    gc.parameter_get_clients={dc.host};gc.parameter_set_clients={dc.host};
    n::ControlNode gateway;assert(!gateway.Start(gc));n::PeerInfo peer;n::NetworkEndpointMetadata endpoint;
    auto select=[&](std::uint64_t old=0){
        Until([&]{
            for(const auto& p:discovery.GetPeers()) {
                if(p.host!=gc.discovery.host||p.state!=n::PeerState::ONLINE||p.session_id==old||!p.metadata_ready)continue;
                for(const auto& e:p.endpoints)if(e.kind==n::EndpointKind::SERVICE){peer=p;endpoint=e;return true;}
            }
            return false;
        });
        Until([&]{for(const auto& p:gateway.GetPeers())if(p.host==dc.host)return true;return false;});
    };select();
    auto descriptor=kcf::TypeDescriptorTraits<Value>::Get();n::TypeIdentity type;assert(!n::ParameterWireType(descriptor,type));
    auto bytes=[&](int count){Value v{count,true,1.5};kcf::DynamicPayload p;p.type_id=type.type_id;p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> wire;assert(!n::EncodeParameterValue(descriptor,p,wire));return wire;};
    auto call=[&](int count,std::chrono::milliseconds timeout=2s){std::this_thread::sleep_for(100ms);return client.CallService(endpoint.identity,peer.session_id,endpoint.name,type,type,bytes(count),timeout);};
    auto denied=call(7);assert(denied.error==-EACCES&&denied.response_received&&!denied.execution_unknown&&calls==0);
    auto old=peer.session_id;gateway.Stop();gc.service_call_clients={dc.host};assert(!gateway.Start(gc));select(old);
    auto result=call(7);assert(!result.error&&result.response_received&&!result.execution_unknown&&calls==1);
    kcf::DynamicPayload decoded;assert(!n::DecodeParameterValue(descriptor,result.value,decoded));Value v{};std::memcpy(&v,decoded.bytes.data(),sizeof(v));assert(v.count==14&&v.enabled&&v.gain==1.5);
    auto reject=[](const n::RemoteServiceCallResult& r,int error){assert(r.error==error&&!r.response_received&&!r.execution_unknown&&r.value.empty());};
    auto wrong=endpoint.identity;++wrong.registration_id;
    reject(client.CallService(wrong,peer.session_id,endpoint.name,type,type,bytes(7)),-ENOENT);
    wrong=endpoint.identity;wrong.runtime.boot_id=Id(91);
    reject(client.CallService(wrong,peer.session_id,endpoint.name,type,type,bytes(7)),-ESTALE);
    reject(client.CallService(endpoint.identity,peer.session_id+1,endpoint.name,type,type,bytes(7)),-ESTALE);
    auto mismatch=type;++mismatch.schema_id;
    reject(client.CallService(endpoint.identity,peer.session_id,endpoint.name,mismatch,type,bytes(7)),-EPROTOTYPE);
    reject(client.CallService(endpoint.identity,peer.session_id,endpoint.name,type,mismatch,bytes(7)),-EPROTOTYPE);
    assert(calls==1);
    auto timeout=call(99,150ms);assert(timeout.error==-ETIMEDOUT&&timeout.execution_unknown&&timeout.value.empty());
    std::this_thread::sleep_for(400ms);assert(calls==2); // no automatic replay/retry
    auto pending=std::async(std::launch::async,[&]{return call(99);});Until([&]{return calls==3;});client.Stop();
    auto canceled=pending.get();assert(canceled.error==-ECANCELED&&canceled.execution_unknown&&!canceled.response_received);
    std::this_thread::sleep_for(400ms);assert(calls==3);assert(!client.Start());
    pending=std::async(std::launch::async,[&]{return call(99);});Until([&]{return calls==4;});gateway.Stop();
    auto disconnected=pending.get();assert(disconnected.error&&disconnected.execution_unknown&&!disconnected.response_received);
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host)return p.state==n::PeerState::LOST;return false;});
    reject(call(7),-ENOTCONN);assert(calls==4);
    old=peer.session_id;assert(!gateway.Start(gc));select(old);
    reject(client.CallService(endpoint.identity,old,endpoint.name,type,type,bytes(7)),-ESTALE);
    result=call(8);assert(!result.error&&calls==5);
    client.Stop();gateway.Stop();discovery.Stop();service.Stop();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Service Client response/count, independent grant, full type/identity, timeout/Stop/disconnect/LOST/no retry PASS\n";
}

void ActionClientTests() {
    auto port=Port();auto dc=Config(61,port);n::PeerDiscovery discovery;assert(!discovery.Start(dc));
    n::RemoteControlClient client(discovery,dc);assert(!client.Start());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> parameter;assert(!parameter.Create("/rcc/concurrent",{1,true,1.5}));
    kcf::Publisher<Value> topic,private_topic;assert(!topic.Create("/rcc/schema_topic"));assert(!private_topic.Create("/rcc/schema_private"));
    kcf::Publisher<DescriptorArray> array_topic;assert(!array_topic.Create("/rcc/schema_array"));
    kcf::ServiceServer service;assert(!service.Create(Port()));std::atomic<int> service_calls{0};
    assert((!service.Register<Value,Value>(30,"/rcc/concurrent_service",[&](const Value& in,Value& out){++service_calls;out=in;out.count*=2;})));
    std::array<kcf::ActionServer<Value,Value,Value>,5> actions;
    std::array<std::atomic<int>,5> calls{},values{};
    n::ControlConfig gc;gc.discovery=Config(60,port);gc.metadata.runtime_pids={getpid()};
    gc.metadata.remote_topics={"/rcc/schema_topic","/rcc/schema_array"};gc.metadata.remote_parameters={"/rcc/concurrent"};gc.metadata.remote_services={"/rcc/concurrent_service"};
    for(unsigned i=0;i<actions.size();++i){auto name="/rcc/action"+std::to_string(i);gc.metadata.remote_actions.push_back(name);
        assert(!actions[i].Create(service,{static_cast<std::uint16_t>(1+i*3),static_cast<std::uint16_t>(2+i*3),static_cast<std::uint16_t>(3+i*3)},name,[&,i](std::uint64_t,const Value& v){++calls[i];values[i]=v.count;return 0;}));}
    assert(!service.Start());kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    std::atomic<bool> stop{false};std::thread worker([&]{std::array<int,5> ticks{},cancel_ticks{};
        while(!stop){for(unsigned i=0;i<actions.size();++i){auto& a=actions[i];auto id=a.GetGoalId();auto state=a.GetState();
            if(a.IsCancelRequested(id)&&(state==kcf::ActionState::ACCEPTED||state==kcf::ActionState::RUNNING)){
                if(++cancel_ticks[i]>=20)assert(!a.Canceled(id,{-1,true,1.5}));
            }else if(state==kcf::ActionState::ACCEPTED){int e=a.Start(id);assert(!e||e==-EINVAL);ticks[i]=cancel_ticks[i]=0;}
            else if(state==kcf::ActionState::RUNNING){++ticks[i];int e=a.PublishFeedback(id,{ticks[i],true,1.5});assert(!e||e==-EAGAIN);
                if(values[i]<100&&ticks[i]>=200)assert(!a.Succeed(id,{values[i]*2,true,1.5}));}
        }std::this_thread::sleep_for(10ms);}});
    gc.parameter_get_clients={dc.host};gc.parameter_set_clients={dc.host};gc.service_call_clients={dc.host};gc.diagnostics_enabled=true;
    n::ControlNode gateway;assert(!gateway.Start(gc));n::PeerInfo peer;
    auto select=[&](std::uint64_t old=0){Until([&]{for(auto& p:discovery.GetPeers())if(p.host==gc.discovery.host&&p.state==n::PeerState::ONLINE&&p.session_id!=old&&p.metadata_ready&&p.endpoints.size()>=7){peer=p;return true;}return false;});
        Until([&]{for(auto& p:gateway.GetPeers())if(p.host==dc.host)return true;return false;});};select();
    auto endpoint=[&](std::string name){for(auto& e:peer.endpoints)if(e.name==name)return e;assert(false);return n::NetworkEndpointMetadata{};};
    auto descriptor=kcf::TypeDescriptorTraits<Value>::Get();n::TypeIdentity type;assert(!n::ParameterWireType(descriptor,type));
    auto bytes=[&](int count){Value v{count,true,1.5};kcf::DynamicPayload p;p.type_id=type.type_id;p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> wire;assert(!n::EncodeParameterValue(descriptor,p,wire));return wire;};
    auto start=[&](int i,int value,std::chrono::milliseconds track=10s){auto e=endpoint(gc.metadata.remote_actions[i]);return client.StartAction(e.identity,peer.session_id,e.name,type,type,type,bytes(value),2s,track);};
    kcf::TypeDescriptor schema;
    auto pe=endpoint("/rcc/concurrent");
    // Default/older Gateway does not advertise the new operation.
    assert(client.QueryTypeDescriptor(pe.identity,peer.session_id,pe.name,n::DescriptorRole::VALUE,type,schema)==-ENOTSUP);
    auto schema_session=peer.session_id;gateway.Stop();gc.descriptor_query_clients={Id(62)};assert(!gateway.Start(gc));select(schema_session);
    assert(client.QueryTypeDescriptor(pe.identity,peer.session_id,pe.name,n::DescriptorRole::VALUE,type,schema)==-EACCES);
    schema_session=peer.session_id;gateway.Stop();gc.descriptor_query_clients={dc.host};assert(!gateway.Start(gc));select(schema_session);
    auto query=[&](const n::NetworkEndpointMetadata& ep,n::DescriptorRole role){kcf::TypeDescriptor result; n::TypeIdentity expected;assert(!n::DescriptorType(ep,role,expected));
        assert(!client.QueryTypeDescriptor(ep.identity,peer.session_id,ep.name,role,expected,result,2s));
        n::TypeIdentity actual;assert(!n::ParameterWireType(result,actual)&&actual==expected);assert(result.payload_alignment==1&&result.payload_size==expected.payload_size);
        return result;};
    schema=query(pe,n::DescriptorRole::VALUE);
    auto se=endpoint("/rcc/concurrent_service");query(se,n::DescriptorRole::REQUEST);query(se,n::DescriptorRole::RESPONSE);
    auto ae=endpoint(gc.metadata.remote_actions[0]);query(ae,n::DescriptorRole::REQUEST);query(ae,n::DescriptorRole::FEEDBACK);query(ae,n::DescriptorRole::RESPONSE);
    auto te=endpoint("/rcc/schema_topic");query(te,n::DescriptorRole::VALUE);
    auto array_schema=query(endpoint("/rcc/schema_array"),n::DescriptorRole::VALUE);assert(array_schema.fields[1].array_count==3);
    auto hidden=endpoint("/rcc/schema_private");
    assert(client.QueryTypeDescriptor(hidden.identity,peer.session_id,hidden.name,n::DescriptorRole::VALUE,type,schema)==-EACCES);
    auto invalid=pe.identity;++invalid.registration_id;
    assert(client.QueryTypeDescriptor(invalid,peer.session_id,pe.name,n::DescriptorRole::VALUE,type,schema)==-ENOENT);
    assert(client.QueryTypeDescriptor(pe.identity,peer.session_id+1,pe.name,n::DescriptorRole::VALUE,type,schema)==-ESTALE);
    auto bad_type=type;++bad_type.schema_id;
    assert(client.QueryTypeDescriptor(pe.identity,peer.session_id,pe.name,n::DescriptorRole::VALUE,bad_type,schema)==-EPROTOTYPE);
    assert(client.QueryTypeDescriptor(pe.identity,peer.session_id,pe.name,n::DescriptorRole::FEEDBACK,type,schema)==-EINVAL);
    // Bypass Client metadata preflight: prove live Gateway gates, not just client checks.
    {using namespace n::client_io;Socket socket;n::ControlFrame hello,reply;hello.source={dc.host,dc.boot_id,getpid(),0};assert(kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks));hello.session_id=discovery.GetSessionId();
        assert(!ConnectHello(socket,dc,peer,hello,reply,Clock::now()+2s,nullptr));
        auto request=hello;request.operation=n::ControlMessage::DESCRIPTOR_REQUEST;request.descriptor_role=n::DescriptorRole::VALUE;request.timeout_ms=1000;request.type=type;
        auto raw=[&](n::NetworkEndpointMetadata ep,n::TypeIdentity expected){static std::uint64_t id=900000;request.request_id=++id;request.endpoint=ep.identity;request.name=ep.name;request.type=expected;
            assert(!SendFrame(socket.fd,request,Clock::now()+2s));assert(!ReceiveFrame(socket.fd,n::ControlMessage::DESCRIPTOR_RESPONSE,reply,Clock::now()+2s));return reply.status;};
        assert(raw(hidden,type)==-EACCES);assert(raw(pe,bad_type)==-EPROTOTYPE);
        // Named storage recreated while metadata is still the old registration.
        assert(!topic.Unlink());topic.Close();assert(!topic.Create("/rcc/schema_topic"));assert(raw(te,type)!=0);
    }
    // Public Topic schema permission must not grant other schema/control access.
    schema_session=peer.session_id;gateway.Stop();
    auto saved=gc;gc.descriptor_query_clients.clear();gc.parameter_get_clients.clear();
    gc.parameter_set_clients.clear();gc.service_call_clients.clear();gc.topic_observer_enabled=true;
    assert(!gateway.Start(gc));select(schema_session);
    query(endpoint("/rcc/schema_topic"),n::DescriptorRole::VALUE);
    for(const auto& e:std::vector<n::NetworkEndpointMetadata>{pe,se,ae}) {
        auto role=e.kind==n::EndpointKind::PARAMETER?n::DescriptorRole::VALUE:n::DescriptorRole::REQUEST;
        n::TypeIdentity expected;assert(!n::DescriptorType(e,role,expected));
        assert(client.QueryTypeDescriptor(e.identity,peer.session_id,e.name,role,expected,schema)==-EACCES);
    }
    assert(client.SetParameter(pe.identity,peer.session_id,pe.name,type,bytes(4)).error!=0);
    assert(client.CallService(se.identity,peer.session_id,se.name,type,type,bytes(4)).error!=0);
    assert(start(0,7).error!=0&&calls[0]==0&&service_calls==0);
    schema_session=peer.session_id;gateway.Stop();gc=saved;assert(!gateway.Start(gc));select(schema_session);
    std::cout<<"Topic Observer denies Parameter/Service/Action schemas and mutation requests PASS\n";
    // New session resets raw request replay window, without changing endpoint APIs.
    schema_session=peer.session_id;gateway.Stop();assert(!gateway.Start(gc));select(schema_session);
    std::cout<<"Descriptor Parameter/Service/Action/Topic roles, permission/default capability, live endpoint/type/scope gates PASS\n";
    auto denied=start(0,7);assert(denied.error==-EACCES&&denied.response_received&&!denied.execution_unknown&&calls[0]==0);
    auto old=peer.session_id;gateway.Stop();gc.action_goal_clients={dc.host};assert(!gateway.Start(gc));select(old);
    auto a=start(0,7);assert(!a.error&&a.handle->Snapshot().accepted);
    auto denied_cancel=client.CancelAction(a.handle);assert(denied_cancel.error==-EACCES&&denied_cancel.response_received&&!denied_cancel.cancel_unknown);
    Until([&]{return a.handle->Snapshot().terminal;});assert(a.handle->Snapshot().state==kcf::ActionState::SUCCEEDED);
    old=peer.session_id;gateway.Stop();gc.action_cancel_clients={dc.host};assert(!gateway.Start(gc));select(old);
    auto e=endpoint(gc.metadata.remote_actions[0]);auto wrong=e.identity;++wrong.registration_id;
    assert(client.StartAction(wrong,peer.session_id,e.name,type,type,type,bytes(1)).error==-ENOENT);
    wrong=e.identity;wrong.runtime.boot_id=Id(99);assert(client.StartAction(wrong,peer.session_id,e.name,type,type,type,bytes(1)).error==-ESTALE);
    assert(client.StartAction(e.identity,peer.session_id+1,e.name,type,type,type,bytes(1)).error==-ESTALE);
    auto bad=type;++bad.schema_id;
    assert(client.StartAction(e.identity,peer.session_id,e.name,type,bad,type,bytes(1)).error==-EPROTOTYPE);
    assert(client.StartAction(e.identity,peer.session_id,e.name,bad,type,type,bytes(1)).error==-EPROTOTYPE);
    assert(client.StartAction(e.identity,peer.session_id,e.name,type,type,bad,bytes(1)).error==-EPROTOTYPE);
    a=start(0,7);auto b=start(1,8);assert(!a.error&&!b.error&&a.handle->Snapshot().goal!=b.handle->Snapshot().goal);
    Until([&]{return a.handle->Snapshot().feedback_sequence&&b.handle->Snapshot().feedback_sequence;});
    auto p=endpoint("/rcc/concurrent"),s=endpoint("/rcc/concurrent_service");std::vector<std::uint8_t> out;
    assert(!client.GetParameter(p.identity,peer.session_id,p.name,type,out,2s));
    assert(!client.SetParameter(p.identity,peer.session_id,p.name,type,bytes(42),2s).error);
    assert(!client.GetParameter(p.identity,peer.session_id,p.name,type,out,2s)&&out==bytes(42));
    auto response=client.CallService(s.identity,peer.session_id,s.name,type,type,bytes(6),2s);assert(!response.error&&response.value==bytes(12)&&service_calls==1);
    n::NetworkDiagnosticsClient diagnostics(discovery,dc);n::NetworkDiagnosticsSnapshot snapshot;assert(!diagnostics.Query(gc.discovery.host,snapshot,2s));
    query(p,n::DescriptorRole::VALUE);query(s,n::DescriptorRole::REQUEST);
    auto cancel=client.CancelAction(b.handle,2s);assert(!cancel.error&&cancel.accepted&&cancel.response_received&&!cancel.cancel_unknown);
    assert(!b.handle->Snapshot().terminal);Until([&]{return b.handle->Snapshot().terminal&&a.handle->Snapshot().terminal;});
    assert(a.handle->Snapshot().state==kcf::ActionState::SUCCEEDED&&a.handle->Snapshot().result==bytes(14));
    assert(b.handle->Snapshot().state==kcf::ActionState::CANCELED&&b.handle->Snapshot().result==bytes(-1));
    assert(calls[0]==2&&calls[1]==1);
    for(int repeat=0;repeat<3;++repeat){std::array<n::RemoteActionStartResult,4> live;
        for(int i=0;i<4;++i){live[i]=start(i,100);assert(!live[i].error);}
        assert(start(4,100).error==-EAGAIN&&calls[4]==0);
        assert(!diagnostics.Query(gc.discovery.host,snapshot,2s));client.Stop();
        for(auto& r:live){auto last=r.handle->Snapshot();assert(last.communication_lost&&last.execution_unknown&&!last.terminal&&last.error==-ECANCELED);}
        assert(!client.Start());assert(client.CancelAction(live[0].handle).error==-ENOENT);
        for(int i=0;i<4;++i)assert(!actions[i].Succeed(actions[i].GetGoalId(),{200,true,1.5}));
        std::this_thread::sleep_for(100ms);
    }
    auto timed=start(0,100,100ms);assert(!timed.error);Until([&]{return timed.handle->Snapshot().error==-ETIMEDOUT;});assert(timed.handle->Snapshot().execution_unknown);
    assert(!actions[0].Succeed(actions[0].GetGoalId(),{200,true,1.5}));
    auto lost=start(1,100);assert(!lost.error);gateway.Stop();Until([&]{return lost.handle->Snapshot().communication_lost;});

    old=peer.session_id;assert(!gateway.Start(gc));select(old);
    // Eight same-incarnation connections, runtime identity mismatch, and ordered
    // replay window on different streams. Ordinary Gateway dedup stays separate.
    {
        using namespace n::client_io;std::array<Socket,8> sockets;
        n::ControlFrame hello;hello.source={dc.host,dc.boot_id,getpid(),0};assert(kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks));hello.session_id=discovery.GetSessionId();
        for(auto& socket:sockets){n::ControlFrame reply;assert(!ConnectHello(socket,dc,peer,hello,reply,Clock::now()+2s,nullptr));}
        Socket extra;n::ControlFrame reply;assert(ConnectHello(extra,dc,peer,hello,reply,Clock::now()+2s,nullptr)!=0);
        auto request=hello;request.operation=n::ControlMessage::PARAM_GET_REQUEST;request.endpoint=p.identity;request.name=p.name;request.type=type;request.timeout_ms=1000;
        auto query=[&](int i,std::uint64_t id){request.request_id=id;assert(!SendFrame(sockets[i].fd,request,Clock::now()+2s));assert(!ReceiveFrame(sockets[i].fd,n::ControlMessage::PARAM_GET_RESPONSE,reply,Clock::now()+2s));assert(reply.request_id==id);return reply.status;};
        assert(!query(0,1000002));assert(!query(1,1000001));assert(query(2,1000001)==-EALREADY);assert(query(3,999000)==-EALREADY);
    }
    std::this_thread::sleep_for(100ms);
    {
        using namespace n::client_io;Socket first,wrong_runtime;n::ControlFrame hello,reply;hello.source={dc.host,dc.boot_id,getpid(),0};assert(kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks));hello.session_id=discovery.GetSessionId();
        assert(!ConnectHello(first,dc,peer,hello,reply,Clock::now()+2s,nullptr));++hello.source.process_start_ticks;
        assert(ConnectHello(wrong_runtime,dc,peer,hello,reply,Clock::now()+2s,nullptr)!=0);
    }
    gateway.Stop();
    client.Stop();stop=true;worker.join();discovery.Stop();service.Stop();
    for(auto& action:actions){action.Unlink();action.Close();}array_topic.Unlink();array_topic.Close();topic.Unlink();topic.Close();private_topic.Unlink();private_topic.Close();parameter.Unlink();parameter.Close();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Action concurrent handles/Get/Set/Service/Diagnostics, ACK/terminal, grants, identity, limits/repeat/Stop/timeout/disconnect PASS\n";
}

void ActionFaultTests(){
    using namespace n::client_io;
    auto port=Port();auto dc=Config(71,port);n::PeerDiscovery discovery;assert(!discovery.Start(dc));n::RemoteControlClient client(discovery,dc);assert(!client.Start());
    int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(!listen(listener,8));fcntl(listener,F_SETFL,fcntl(listener,F_GETFL)|O_NONBLOCK);
    auto sc=Config(70,port);sc.metadata_port=tcp;sc.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::REMOTE_ACTION)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
    n::NetworkEndpointMetadata e;e.identity={{sc.host,sc.boot_id,getpid(),1},1};e.name="/fake/action";e.element_name="fake";e.kind=n::EndpointKind::ACTION;e.scope=n::NetworkScope::REMOTE;
    n::TypeIdentity type;assert(!n::ParameterWireType(kcf::TypeDescriptorTraits<Value>::Get(),type));e.request_type=e.response_type=e.feedback_type=type;
    n::PeerDiscovery fake;assert(!fake.Start(sc,{e}));n::PeerInfo peer;
    Until([&]{for(auto& p:discovery.GetPeers())if(p.host==sc.host&&p.metadata_ready){peer=p;return true;}return false;});
    // Missing ACK, Cancel timeout, Stop during Cancel, wrong Goal correlation,
    // peer LOST with a live socket, and changed session with a live old socket.
    for(int scenario=0;scenario<6;++scenario){std::atomic<bool> received{false},release{false};
        std::thread server([&]{Socket socket;Until([&]{socket.fd=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);return socket.fd>=0;});
            auto end=Clock::now()+4s;n::ControlFrame hello,request;assert(!ReceiveFrame(socket.fd,n::ControlMessage::HELLO,hello,end));hello.source=e.identity.runtime;hello.session_id=fake.GetSessionId();assert(!SendFrame(socket.fd,hello,end));
            assert(!ReceiveFrame(socket.fd,n::ControlMessage::ACTION_GOAL_REQUEST,request,end));
            if(scenario){auto ack=request;ack.source=hello.source;ack.session_id=hello.session_id;ack.operation=n::ControlMessage::ACTION_GOAL_RESPONSE;ack.timeout_ms=0;ack.payload.clear();ack.action_state=kcf::ActionState::ACCEPTED;
                // Fragment real Action ACK to exercise the client's stream state.
                std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(ack,bytes));for(auto byte:bytes){assert(send(socket.fd,&byte,1,MSG_NOSIGNAL)==1);std::this_thread::sleep_for(50us);}
                if(scenario==1||scenario==2){n::ControlFrame cancel;assert(!ReceiveFrame(socket.fd,n::ControlMessage::ACTION_CANCEL_REQUEST,cancel,end));assert(cancel.goal==request.goal);}
                if(scenario==3){auto feedback=ack;feedback.operation=n::ControlMessage::ACTION_FEEDBACK;feedback.request_id=0;feedback.action_state=kcf::ActionState::RUNNING;feedback.feedback_sequence=1;feedback.payload.resize(type.payload_size);++feedback.goal.goal_id;assert(!SendFrame(socket.fd,feedback,end));}
            }
            received=true;Until([&]{return release.load();});
        });
        auto started=client.StartAction(e.identity,peer.session_id,e.name,type,type,type,std::vector<std::uint8_t>(type.payload_size),scenario==0?200ms:2s,5s);
        if(scenario==0){assert(started.error==-ETIMEDOUT&&started.execution_unknown&&!started.response_received);}
        else {assert(!started.error);if(scenario==1||scenario==2){auto pending=std::async(std::launch::async,[&]{return client.CancelAction(started.handle,scenario==1?150ms:2s);});Until([&]{return received.load();});if(scenario==2)client.Stop();auto canceled=pending.get();assert(canceled.cancel_unknown&&!canceled.response_received&&canceled.error==(scenario==1?-ETIMEDOUT:-ECANCELED));}
            else if(scenario==3){Until([&]{return started.handle->Snapshot().error==-EPROTO;});}
            else {fake.Stop();if(scenario==5){assert(!fake.Start(sc,{e}));}Until([&]{return started.handle->Snapshot().communication_lost;});assert(started.handle->Snapshot().execution_unknown);}
        }
        release=true;server.join();client.Stop();assert(!client.Start());
        if(scenario==4){assert(!fake.Start(sc,{e}));Until([&]{for(auto& p:discovery.GetPeers())if(p.host==sc.host&&p.metadata_ready&&p.session_id!=peer.session_id){peer=p;return true;}return false;});}
    }
    client.Stop();fake.Stop();discovery.Stop();close(listener);
    std::cout<<"Action missing ACK, fragmented ACK, Cancel timeout/Stop unknown, wrong Goal, LOST/session change PASS\n";
}

void DescriptorCodecTests(){
    struct ArrayValue{bool enabled;std::int32_t samples[3];double gain;};
    auto d=kcf::MakeTypeDescriptor<ArrayValue>("test.descriptor.array.v1");d.field_count=3;
    d.fields[0]=kcf::MakeField<ArrayValue,bool>("enabled",offsetof(ArrayValue,enabled));
    d.fields[1]=kcf::MakeField<ArrayValue,std::int32_t[3]>("samples",offsetof(ArrayValue,samples));
    d.fields[2]=kcf::MakeField<ArrayValue,double>("gain",offsetof(ArrayValue,gain));
    n::TypeIdentity type;assert(!n::ParameterWireType(d,type));std::vector<std::uint8_t> bytes;assert(!n::EncodeTypeDescriptor(d,bytes));
    kcf::TypeDescriptor packed;assert(!n::DecodeTypeDescriptor(bytes,type,packed));assert(packed.fields[1].array_count==3&&packed.fields[1].offset==1&&packed.payload_size==21);
    ArrayValue value{true,{1,-2,3},1.5};kcf::DynamicPayload native;native.type_id=d.type_id;native.bytes.resize(sizeof(value));memcpy(native.bytes.data(),&value,sizeof(value));
    std::vector<std::uint8_t> wire,roundtrip;assert(!n::EncodeParameterValue(d,native,wire));kcf::DynamicPayload generic;assert(!n::DecodeParameterValue(packed,wire,generic));assert(!n::EncodeParameterValue(packed,generic,roundtrip)&&roundtrip==wire);
    auto original=packed;
    for(std::size_t i=0;i<bytes.size();++i){auto truncated=bytes;truncated.resize(i);assert(n::DecodeTypeDescriptor(truncated,type,packed));assert(!memcmp(&packed,&original,sizeof(packed)));}
    auto invalid=bytes;invalid.push_back(0);assert(n::DecodeTypeDescriptor(invalid,type,packed));
    invalid.assign(n::MAX_PARAMETER_WIRE+1,0);assert(n::DecodeTypeDescriptor(invalid,type,packed)==-EMSGSIZE);
    auto wrong=type;++wrong.schema_id;assert(n::DecodeTypeDescriptor(bytes,wrong,packed)==-EPROTOTYPE);
    // Locate first field after schema version, type name and field count.
    auto first=6+std::strlen(d.type_name);auto kind=first+2+std::strlen(d.fields[0].name);
    invalid=bytes;invalid[kind]=255;assert(n::DecodeTypeDescriptor(invalid,type,packed));
    invalid=bytes;for(int i=1;i<=4;++i)invalid[kind+i]=255;assert(n::DecodeTypeDescriptor(invalid,type,packed));
    invalid=bytes;invalid[first]=255;assert(n::DecodeTypeDescriptor(invalid,type,packed));
    invalid=bytes;invalid[first-2]=255;assert(n::DecodeTypeDescriptor(invalid,type,packed));
    auto broken=d;std::strcpy(broken.fields[1].name,broken.fields[0].name);assert(n::EncodeTypeDescriptor(broken,invalid));
    broken=d;broken.fields[1].array_count=UINT32_MAX;assert(n::EncodeTypeDescriptor(broken,invalid));
    // Duplicate field on untrusted wire: equal-length names allow direct replacement.
    auto two=kcf::MakeTypeDescriptor<ArrayValue>("test.duplicate.v1");two.field_count=2;two.fields[0]=d.fields[0];two.fields[1]=d.fields[2];std::strcpy(two.fields[0].name,"one");std::strcpy(two.fields[1].name,"two");
    assert(!n::EncodeTypeDescriptor(two,invalid));auto name=6+std::strlen(two.type_name)+2;auto next=name+3+1+4+2;std::copy(invalid.begin()+name,invalid.begin()+name+3,invalid.begin()+next);assert(n::DecodeTypeDescriptor(invalid,type,packed)==-EPROTO);
    n::ControlFrame frame;frame.source={Id(1),Id(2),123,1};frame.session_id=1;frame.request_id=1;frame.endpoint={{Id(3),Id(4),456,1},1};frame.name="/schema";frame.type=type;frame.descriptor_role=n::DescriptorRole::VALUE;frame.operation=n::ControlMessage::DESCRIPTOR_RESPONSE;frame.payload=bytes;
    std::vector<std::uint8_t> encoded;n::ControlFrame decoded;assert(!n::EncodeControlFrame(frame,encoded));assert(!n::DecodeControlFrame(encoded.data(),encoded.size(),decoded));
    auto bad_frame=encoded;bad_frame[172]=99;assert(n::DecodeControlFrame(bad_frame.data(),bad_frame.size(),decoded));bad_frame=encoded;bad_frame[173]=1;assert(n::DecodeControlFrame(bad_frame.data(),bad_frame.size(),decoded));
    frame.operation=n::ControlMessage::DESCRIPTOR_REQUEST;frame.timeout_ms=1000;frame.payload.clear();assert(!n::EncodeControlFrame(frame,encoded));frame.descriptor_role=n::DescriptorRole::NONE;assert(n::EncodeControlFrame(frame,encoded));
    std::cout<<"Schema scalar/fixed array, canonical roundtrip, truncation/length/count/kind/overflow/duplicate/identity checks PASS\n";
}
void PartialFrameTests(){
    int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,fd));
    n::ControlFrame hello;hello.source={Id(1),Id(2),123,456};hello.session_id=1;
    std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(hello,bytes));n::client_io::FrameReader reader;n::ControlFrame out;
    for(int repeat=0;repeat<3;++repeat)for(std::size_t i=0;i<bytes.size();++i){assert(write(fd[0],&bytes[i],1)==1);int code=reader.Read(fd[1],out);assert(code==(i+1==bytes.size()?1:0));}
    assert(out.source==hello.source);close(fd[0]);assert(reader.Read(fd[1],out)==-ECONNRESET);close(fd[1]);
    std::cout<<"Incremental TCP prefix/body, consecutive frames and disconnect PASS\n";
}
int main(){
    alarm(120);std::cout.setf(std::ios::unitbuf);DescriptorCodecTests();PartialFrameTests();ActionFaultTests();ActionClientTests();ServiceClientTests();
    auto port=Port();auto dc=Config(20,port);n::PeerDiscovery discovery;assert(!discovery.Start(dc));
    n::RemoteControlClient client(discovery,dc);assert(!client.Start());assert(client.Start()==-EBUSY);
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> parameter;assert(!parameter.Create("/rcc/value",{42,true,1.5}));
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlConfig gc;gc.discovery=Config(10,port);gc.metadata.runtime_pids={getpid()};gc.metadata.remote_parameters={"/rcc/value"};gc.parameter_get_clients={dc.host};gc.diagnostics_enabled=true;
    n::ControlNode gateway;assert(!gateway.Start(gc));n::PeerInfo peer;
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host&&p.metadata_ready&&!p.endpoints.empty()){peer=p;return true;}return false;});
    Until([&]{for(const auto& p:gateway.GetPeers())if(p.host==dc.host)return true;return false;});
    auto e=peer.endpoints.front();auto descriptor=kcf::TypeDescriptorTraits<Value>::Get();n::TypeIdentity type;assert(!n::ParameterWireType(descriptor,type));
    std::vector<std::uint8_t> out{99};
    assert(!client.GetParameter(e.identity,peer.session_id,e.name,type,out,2s));
    kcf::DynamicPayload decoded;assert(!n::DecodeParameterValue(descriptor,out,decoded));Value v{};std::memcpy(&v,decoded.bytes.data(),sizeof(v));assert(v.count==42&&v.enabled&&v.gain==1.5);
    auto unchanged=out;auto wrong=e.identity;++wrong.registration_id;
    assert(client.GetParameter(wrong,peer.session_id,e.name,type,out)==-ENOENT);
    auto wrong_type=type;++wrong_type.type_id;assert(client.GetParameter(e.identity,peer.session_id,e.name,wrong_type,out)==-EPROTOTYPE);
    assert(client.GetParameter(e.identity,peer.session_id+1,e.name,type,out)==-ESTALE);
    wrong=e.identity;wrong.runtime.boot_id=Id(81);assert(client.GetParameter(wrong,peer.session_id,e.name,type,out)==-ESTALE);
    assert(out==unchanged);std::cout<<"Get canonical bytes, endpoint/type/boot/session rejection PASS\n";
    std::this_thread::sleep_for(100ms);
    n::NetworkDiagnosticsClient diagnostics(discovery,dc);n::NetworkDiagnosticsSnapshot snapshot;assert(!diagnostics.Query(gc.discovery.host,snapshot,2s));
    std::this_thread::sleep_for(100ms);client.Stop();assert(client.GetParameter(e.identity,peer.session_id,e.name,type,out)==-ENOTCONN);assert(!client.Start());
    assert(!client.GetParameter(e.identity,peer.session_id,e.name,type,out,2s));std::cout<<"Shared discovery Diagnostics and client Stop/Start PASS\n";
    assert(Raw(discovery,dc,peer,e,n::ControlMessage::PARAM_GET_REQUEST,true)==-EPROTOTYPE);
    assert(Raw(discovery,dc,peer,e,n::ControlMessage::PARAM_SET_REQUEST)==-EACCES);
    assert(Raw(discovery,dc,peer,e,n::ControlMessage::PARAM_GET_REQUEST,false,true)==-ECONNRESET);
    Value preserved{};assert(!parameter.Get(preserved)&&preserved.count==42);
    std::cout<<"Gateway wrong type, Set and broad target rejection PASS\n";
    std::this_thread::sleep_for(100ms);
    auto denied_set=client.SetParameter(e.identity,peer.session_id,e.name,type,out);
    assert(denied_set.error==-EACCES&&!denied_set.execution_unknown&&denied_set.response_received);
    assert(!parameter.Get(preserved)&&preserved.count==42);
    // Restart with an independent Set allowlist, then select its new session.
    gateway.Stop();gc.parameter_set_clients={dc.host};assert(!gateway.Start(gc));
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host&&p.state==n::PeerState::ONLINE&&p.metadata_ready&&p.session_id!=peer.session_id){peer=p;return true;}return false;});
    Until([&]{for(const auto& p:gateway.GetPeers())if(p.host==dc.host)return true;return false;});
    Value changed{99,false,2.5};kcf::DynamicPayload native;native.type_id=type.type_id;native.bytes.resize(sizeof(changed));std::memcpy(native.bytes.data(),&changed,sizeof(changed));
    std::vector<std::uint8_t> wire;assert(!n::EncodeParameterValue(descriptor,native,wire));
    auto set=client.SetParameter(e.identity,peer.session_id,e.name,type,wire,2s);
    assert(!set.error&&!set.execution_unknown&&set.response_received);
    std::this_thread::sleep_for(100ms);
    assert(!client.GetParameter(e.identity,peer.session_id,e.name,type,out,2s));assert(out==wire);
    assert(!parameter.Get(preserved)&&preserved.count==99&&!preserved.enabled&&preserved.gain==2.5);
    auto reject=[&](n::RemoteParameterSetResult r,int error){assert(r.error==error&&!r.execution_unknown&&!r.response_received);};
    wrong=e.identity;++wrong.registration_id;reject(client.SetParameter(wrong,peer.session_id,e.name,type,wire),-ENOENT);
    reject(client.SetParameter(e.identity,peer.session_id,e.name,wrong_type,wire),-EPROTOTYPE);
    reject(client.SetParameter(e.identity,peer.session_id+1,e.name,type,wire),-ESTALE);
    wrong=e.identity;wrong.runtime.boot_id=Id(81);reject(client.SetParameter(wrong,peer.session_id,e.name,type,wire),-ESTALE);
    std::cout<<"Get/Set/Get, independent Set grant and Set identity/type gates PASS\n";
    gateway.Stop();Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host)return p.state==n::PeerState::LOST;return false;});
    assert(client.GetParameter(e.identity,peer.session_id,e.name,type,out)==-ENOTCONN);
    reject(client.SetParameter(e.identity,peer.session_id,e.name,type,wire),-ENOTCONN);
    assert(!gateway.Start(gc));Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host)if(p.state==n::PeerState::ONLINE&&p.metadata_ready&&p.session_id!=peer.session_id)return true;return false;});
    assert(client.GetParameter(e.identity,peer.session_id,e.name,type,out)==-ESTALE);std::cout<<"LOST/restart old selection rejection PASS\n";
    for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host)peer=p;
    gateway.Stop();gc.parameter_get_clients.clear();gc.parameter_set_clients.clear();assert(!gateway.Start(gc));
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==gc.discovery.host&&p.state==n::PeerState::ONLINE&&p.metadata_ready&&p.session_id!=peer.session_id){peer=p;return true;}return false;});
    int denied=client.GetParameter(e.identity,peer.session_id,e.name,type,out,500ms);assert(denied==-ECONNRESET);std::cout<<"Default gateway denies dedicated Get PASS\n";
    gateway.Stop();
    int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(!listen(listener,8));
    n::PeerDiscovery silent;auto sc=Config(30,port);sc.metadata_port=tcp;sc.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::REMOTE_PARAMETER)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
    auto fake=e;fake.identity.runtime.host=sc.host;fake.identity.runtime.boot_id=sc.boot_id;assert(!silent.Start(sc,{fake}));n::PeerInfo sp;
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==sc.host&&p.metadata_ready){sp=p;return true;}return false;});
    assert(client.GetParameter(fake.identity,sp.session_id,fake.name,type,out,100ms)==-ETIMEDOUT);
    reject(client.SetParameter(fake.identity,sp.session_id,fake.name,type,wire,100ms),-ETIMEDOUT);
    auto waiting=std::async(std::launch::async,[&]{std::vector<std::uint8_t> bytes;return client.GetParameter(fake.identity,sp.session_id,fake.name,type,bytes,10s);});
    std::this_thread::sleep_for(50ms);
    auto queued=std::async(std::launch::async,[&]{std::vector<std::uint8_t> bytes;return client.GetParameter(fake.identity,sp.session_id,fake.name,type,bytes,10s);});
    std::this_thread::sleep_for(30ms);auto before=std::chrono::steady_clock::now();client.Stop();assert(waiting.get()==-ECANCELED);assert(queued.get()==-ECANCELED);assert(std::chrono::steady_clock::now()-before<500ms);
    std::cout<<"Timeout and Stop active/queued requests PASS\n";
    silent.Stop();sc.profile.network.major=2;assert(!silent.Start(sc,{fake}));assert(!client.Start());
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==sc.host&&p.profile.network.major==2){sp=p;return true;}return false;});
    assert(client.GetParameter(fake.identity,sp.session_id,fake.name,type,out)==-EPROTONOSUPPORT);
    silent.Stop();sc.profile.network.major=1;assert(!silent.Start(sc,{fake}));
    Until([&]{for(const auto& p:discovery.GetPeers())if(p.host==sc.host&&p.profile.network.major==1&&p.metadata_ready){sp=p;return true;}return false;});
    // Drain earlier HELLO-timeout sockets from the silent peer's listener.
    int flags=fcntl(listener,F_GETFL);fcntl(listener,F_SETFL,flags|O_NONBLOCK);
    for(;;){int fd=accept(listener,nullptr,nullptr);if(fd<0)break;close(fd);}
    for(int scenario=0;scenario<3;++scenario){
        std::atomic<bool> received{false};
        std::thread server([&]{
            using namespace n::client_io;
            Socket socket;Until([&]{socket.fd=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);return socket.fd>=0;});
            auto deadline=Clock::now()+3s;n::ControlFrame hello;assert(!ReceiveFrame(socket.fd,n::ControlMessage::HELLO,hello,deadline));
            hello.source=fake.identity.runtime;hello.session_id=silent.GetSessionId();assert(!SendFrame(socket.fd,hello,deadline));
            n::ControlFrame request;assert(!ReceiveFrame(socket.fd,n::ControlMessage::PARAM_SET_REQUEST,request,deadline));assert(request.payload==wire);received=true;
            if(scenario!=1)std::this_thread::sleep_for(350ms);
        });
        auto pending=std::async(std::launch::async,[&]{return client.SetParameter(fake.identity,sp.session_id,fake.name,type,wire,scenario==0?150ms:2s);});
        Until([&]{return received.load();});if(scenario==2)client.Stop();
        auto result=pending.get();assert(result.execution_unknown&&!result.response_received);
        assert(result.error==(scenario==0?-ETIMEDOUT:scenario==1?-ECONNRESET:-ECANCELED));server.join();
    }
    std::cout<<"Set after-dispatch timeout/disconnect/Stop execution_unknown PASS\n";
    client.Stop();silent.Stop();close(listener);discovery.Stop();parameter.Unlink();parameter.Close();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Remote Control Client PASS\n";
}
