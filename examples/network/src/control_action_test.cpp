#include "kcf/network/control_node.hpp"
#include "kcf/action/action_server.hpp"
#include "kcf/dynamic/dynamic_action_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/detail/supervisor_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
namespace n=kcf::network;
using namespace std::chrono_literals;
n::HostIdentity Id(unsigned char x){n::HostIdentity id;id.bytes[0]=x;return id;}
auto Descriptor(){return kcf::TypeDescriptorTraits<Value>::Get();}
n::TypeIdentity Type(){n::TypeIdentity t;assert(!n::ParameterWireType(Descriptor(),t));return t;}
std::vector<std::uint8_t> Bytes(int count){Value value{count,true,1.5};kcf::DynamicPayload p;p.type_id=Type().type_id;p.bytes.resize(sizeof(value));std::memcpy(p.bytes.data(),&value,sizeof(value));std::vector<std::uint8_t> out;assert(!n::EncodeParameterValue(Descriptor(),p,out));return out;}
int Count(const std::vector<std::uint8_t>& wire){kcf::DynamicPayload p;assert(!n::DecodeParameterValue(Descriptor(),wire,p));Value v;std::memcpy(&v,p.bytes.data(),sizeof(v));return v.count;}
template<class F>void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(5ms);}}
std::uint16_t Port(int kind=SOCK_DGRAM,int* keep=nullptr){int fd=socket(AF_INET,kind|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t length=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&length));if(keep)*keep=fd;else close(fd);return ntohs(a.sin_port);}
n::ControlConfig Config(unsigned char host,std::uint16_t port){n::ControlConfig c;c.discovery.host=Id(host);c.discovery.boot_id=Id(42);c.discovery.port=port;c.discovery.interface_address="127.0.0.1";c.discovery.announce_interval=50ms;c.discovery.peer_timeout=350ms;c.discovery.metadata_retry=30ms;c.metadata_interval=30ms;c.connect_timeout=300ms;c.reconnect_interval=50ms;c.metadata.runtime_pids={-1};return c;}
n::RemoteTarget Host(unsigned char host){n::RemoteTarget t;t.scope=n::TargetScope::HOST;t.host=Id(host);return t;}
n::RemoteTarget Endpoint(const n::NetworkEndpointMetadata& e){n::RemoteTarget t;t.endpoint=e.identity;return t;}
n::PeerInfo Peer(n::ControlNode& node,unsigned char host){for(const auto& p:node.GetPeers())if(p.host==Id(host))return p;return {};}
n::ActionBatchResult Result(std::future<n::ActionBatchResult> f){assert(f.wait_for(5s)==std::future_status::ready);return f.get();}
n::ParameterEndpointResult One(std::future<n::ActionBatchResult> f){auto r=Result(std::move(f));static unsigned step=0;++step;if(r.error||r.endpoints.size()!=1)std::cerr<<"One step="<<step<<" error="<<r.error<<" endpoints="<<r.endpoints.size()<<std::endl;assert(!r.error&&r.endpoints.size()==1);return r.endpoints[0];}
volatile std::sig_atomic_t stopping=0,remove_action=0;
void Signal(int s){if(s==SIGUSR1)remove_action=1;else stopping=1;}
int Owner(std::uint16_t port){alarm(120);std::signal(SIGTERM,Signal);std::signal(SIGUSR1,Signal);
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::ServiceServer server;assert(!server.Create(port));
    kcf::ActionServer<Value,Value,Value> action,local;std::atomic<int> count{0};
    const auto name="/kcf_action_"+std::to_string(getpid());
    assert(!action.Create(server,{1,2,3},name,[&](std::uint64_t,const Value& value){if(value.count<0)return -EINVAL;if(value.count==99)std::this_thread::sleep_for(350ms);count=value.count;return 0;}));
    assert(!local.Create(server,{4,5,6},name+"_local",[](std::uint64_t,const Value&){return -EINVAL;}));
    assert(!server.Start());kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    int ticks=0;
    while(!stopping){
        if(remove_action){server.Stop();remove_action=0;}
        auto id=action.GetGoalId();auto state=action.GetState();
        if(action.IsCancelRequested(id)&&(state==kcf::ActionState::ACCEPTED||state==kcf::ActionState::RUNNING)){assert(!action.Canceled(id,{-1,true,1}));}
        else if(state==kcf::ActionState::ACCEPTED){assert(!action.Start(id));ticks=0;}
        else if(state==kcf::ActionState::RUNNING){++ticks;for(int i=0;i<(count==77?100:1);++i){int code=action.PublishFeedback(id,{ticks,true,1});assert(!code||code==-EAGAIN);}
            if(ticks>=50)assert(!action.Succeed(id,{count.load()*2,true,1}));}
        std::this_thread::sleep_for(10ms);
    }
    server.Stop();action.Unlink();local.Unlink();action.Close();local.Close();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);return 0;
}
void Codec(){n::ControlFrame f;f.operation=n::ControlMessage::ACTION_GOAL_REQUEST;f.source={Id(1),Id(42),123,1};f.session_id=1;f.request_id=2;f.endpoint={{Id(2),Id(42),321,1},1ull<<62};f.name="/action";f.type=Type();f.feedback_type=Type();f.response_type=Type();f.goal={f.source,9,1};f.payload=Bytes(6);f.timeout_ms=100;
    std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(f,bytes)&&bytes.size()==292+f.name.size()+13);n::ControlFrame out;assert(!n::DecodeControlFrame(bytes.data(),bytes.size(),out)&&out.goal==f.goal);
    for(std::size_t i=0;i<bytes.size();++i)assert(n::DecodeControlFrame(bytes.data(),i,out));
    for(auto offset:{0,5,7,11,15,282}){auto bad=bytes;bad[offset]=0xff;assert(n::DecodeControlFrame(bad.data(),bad.size(),out));}
    f.goal.source.host=Id(9);assert(n::EncodeControlFrame(f,bytes));f.goal.source=f.source;
    ++f.goal.session_id;assert(n::EncodeControlFrame(f,bytes));--f.goal.session_id;
    for(auto op:{n::ControlMessage::ACTION_GOAL_RESPONSE,n::ControlMessage::ACTION_FEEDBACK,n::ControlMessage::ACTION_RESULT,n::ControlMessage::ACTION_CANCEL_REQUEST,n::ControlMessage::ACTION_CANCEL_RESPONSE}){
        f.operation=op;f.timeout_ms=op==n::ControlMessage::ACTION_CANCEL_REQUEST?100:0;f.payload.clear();f.feedback_sequence=0;f.request_id=2;f.action_state=kcf::ActionState::ACCEPTED;
        if(op==n::ControlMessage::ACTION_CANCEL_REQUEST)f.action_state=kcf::ActionState::IDLE;
        if(op==n::ControlMessage::ACTION_FEEDBACK){f.request_id=0;f.action_state=kcf::ActionState::RUNNING;f.feedback_sequence=1;f.payload=Bytes(1);}
        if(op==n::ControlMessage::ACTION_RESULT){f.request_id=0;f.action_state=kcf::ActionState::SUCCEEDED;f.payload=Bytes(2);}
        assert(!n::EncodeControlFrame(f,bytes)&&!n::DecodeControlFrame(bytes.data(),bytes.size(),out));
    }
    std::cout<<"Action operations and malformed frame/identity/size checks PASS\n";
}
void Integration(const char* exe){auto port=Port();pid_t children[2];std::string names[2];
    for(int i=0;i<2;++i){auto local=std::to_string(Port());children[i]=fork();assert(children[i]>=0);if(!children[i]){execl(exe,exe,"--owner",local.c_str(),nullptr);_exit(127);}names[i]="/kcf_action_"+std::to_string(children[i]);}
    kcf::detail::SupervisorRegistry supervisor;supervisor.Begin();kcf::SupervisorInfo app{};std::strcpy(app.application_name,"action_test");app.application_state=kcf::ApplicationState::RUNNING;app.element_count=2;
    for(int i=0;i<2;++i){auto& e=app.elements[i];e.pid=children[i];e.process_alive=1;std::strcpy(e.name,i?"second":"first");std::strcpy(e.executable,exe);Until([&]{return kcf::detail::ReadProcessIdentity(e.pid,e.process_start_ticks);});}supervisor.Update(app);
    n::ControlNode a,b,c;auto ca=Config(10,port),cb=Config(20,port),cc=Config(30,port);ca.metadata.runtime_pids={children[0],children[1]};ca.metadata.remote_actions={names[0],names[1]};ca.metadata.groups={"robots"};assert(!a.Start(ca)&&!b.Start(cb)&&!c.Start(cc));
    Until([&]{auto p=Peer(b,10);return p.metadata_ready&&p.endpoints.size()==4;});auto peer=Peer(b,10);n::NetworkEndpointMetadata endpoints[2];
    for(const auto& e:peer.endpoints){assert(e.kind==n::EndpointKind::ACTION&&e.identity.registration_id&(1ull<<62));assert(e.application_name=="action_test");for(int i=0;i<2;++i)if(e.name==names[i])endpoints[i]=e;}
    assert(n::Valid(endpoints[0])&&n::Valid(endpoints[1]));
    assert(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_ACTION));assert(!(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_TOPIC)));
    auto start=[&](const n::RemoteTarget& target,int v,int index=0){return b.StartAction(target,names[index],Type(),Type(),Type(),Bytes(v),2s);};
    auto first=One(start(Endpoint(endpoints[0]),5));assert(!first.error&&first.action&&first.action->Snapshot().accepted);
    std::uint64_t previous=0;int feedbacks=0;Until([&]{auto s=first.action->Snapshot();if(s.feedback_sequence!=previous){assert(s.feedback_sequence>previous);previous=s.feedback_sequence;++feedbacks;assert(Count(s.feedback)>0);}return s.terminal;});
    assert(feedbacks>=2&&Count(first.action->Snapshot().result)==10&&first.action->Snapshot().state==kcf::ActionState::SUCCEEDED);
    auto rejected=One(start(Endpoint(endpoints[0]),-1));assert(rejected.error==-EINVAL&&!rejected.action->Snapshot().accepted&&!rejected.execution_unknown);
    auto cancel=One(start(Endpoint(endpoints[0]),7));assert(!cancel.error);auto cancel_result=One(b.CancelAction(cancel.action));assert(!cancel_result.error&&!cancel_result.cancel_unknown);Until([&]{return cancel.action->Snapshot().terminal;});assert(cancel.action->Snapshot().state==kcf::ActionState::CANCELED);
    assert(Result(c.CancelAction(cancel.action)).error==-EACCES);
    for(auto scope:{n::TargetScope::HOST,n::TargetScope::APPLICATION,n::TargetScope::GROUP,n::TargetScope::ALL}){n::RemoteTarget t;t.scope=scope;if(scope==n::TargetScope::HOST)t.host=Id(10);if(scope==n::TargetScope::APPLICATION)t.application=endpoints[0].application;if(scope==n::TargetScope::GROUP)t.group="robots";auto r=One(start(t,4));assert(!r.error);Until([&]{return r.action->Snapshot().terminal;});assert(Count(r.action->Snapshot().result)==8);}
    // Same numeric IDs from different Host identities are distinct.
    auto left=One(start(Endpoint(endpoints[0]),8));auto right=One(c.StartAction(Endpoint(endpoints[1]),names[1],Type(),Type(),Type(),Bytes(9),2s));assert(!left.error&&!right.error&&left.action->Snapshot().goal!=right.action->Snapshot().goal);
    Until([&]{return left.action->Snapshot().terminal&&right.action->Snapshot().terminal;});assert(Count(left.action->Snapshot().result)==16&&Count(right.action->Snapshot().result)==18);
    n::ControlNode d,e;assert(!d.Start(Config(40,port))&&!e.Start(Config(50,port)));
    Until([&]{return Peer(d,10).metadata_ready&&Peer(e,10).metadata_ready;});
    auto dgoal=One(d.StartAction(Endpoint(endpoints[0]),names[0],Type(),Type(),Type(),Bytes(4),2s));
    auto egoal=One(e.StartAction(Endpoint(endpoints[1]),names[1],Type(),Type(),Type(),Bytes(5),2s));
    assert(!dgoal.error&&!egoal.error);auto di=dgoal.action->Snapshot().goal,ei=egoal.action->Snapshot().goal;
    assert(di.goal_id==ei.goal_id&&di.source.pid==ei.source.pid&&di.source.process_start_ticks==ei.source.process_start_ticks&&di.source.host!=ei.source.host&&di!=ei);
    Until([&]{return dgoal.action->Snapshot().terminal&&egoal.action->Snapshot().terminal;});d.Stop();e.Stop();
    auto wrong=Type();++wrong.type_id;
    for(int i=0;i<3;++i){auto r=One(b.StartAction(Endpoint(endpoints[0]),names[0],i==0?wrong:Type(),i==1?wrong:Type(),i==2?wrong:Type(),Bytes(1)));assert(r.reason==n::CompatibilityReason::TYPE_MISMATCH&&!r.execution_unknown);}
    auto local=One(b.StartAction(Host(10),names[0]+"_local",Type(),Type(),Type(),Bytes(1)));assert(local.reason==n::CompatibilityReason::LOCAL_ONLY);
    auto bad=Bytes(1);bad[4]=2;assert(One(b.StartAction(Endpoint(endpoints[0]),names[0],Type(),Type(),Type(),bad)).error==-EPROTO);
    auto flood=One(start(Endpoint(endpoints[0]),77));assert(!flood.error);Until([&]{return flood.action->Snapshot().terminal;});assert(flood.action->Snapshot().feedback_sequence>100&&Count(flood.action->Snapshot().result)==154);
    // Goal acceptance delayed beyond request timeout; no automatic retry.
    auto timeout=One(b.StartAction(Endpoint(endpoints[0]),names[0],Type(),Type(),Type(),Bytes(99),80ms));assert(timeout.error==-ETIMEDOUT&&timeout.execution_unknown);
    std::this_thread::sleep_for(1100ms);
    // Disconnect releases tracking; independently inspect Local result to prove no cancel.
    auto running=One(start(Endpoint(endpoints[0]),11));assert(!running.error);
    n::MetadataConfig metadata=ca.metadata;metadata.host=Id(10);metadata.boot_id=Id(42);std::vector<n::LocalEndpoint> entries;assert(!n::CollectLocalMetadata(metadata,entries));n::LocalEndpoint entry;for(const auto& e:entries)if(e.metadata.identity==endpoints[0].identity)entry=e;
    kcf::DynamicActionClient direct;assert(!direct.Open(entry.action,entry.descriptor,entry.feedback_descriptor,entry.response_descriptor,children[0],entry.metadata.identity.runtime.process_start_ticks));kcf::ActionHeader header;kcf::DynamicPayload value;Until([&]{return !direct.ReadFeedback(header,value)&&header.state==kcf::ActionState::RUNNING;});auto local_goal=header.goal_id;
    b.Stop();assert(running.action->Snapshot().communication_lost);kcf::ActionState state;Until([&]{int code=direct.GetResult(local_goal,state,value);assert(!code||code==-EINPROGRESS);return !code;});assert(state==kcf::ActionState::SUCCEEDED);direct.Close();
    assert(!b.Start(cb));Until([&]{return Peer(b,10).metadata_ready;});auto fresh=One(start(Endpoint(endpoints[0]),3));assert(!fresh.error&&fresh.action->Snapshot().goal!=running.action->Snapshot().goal);Until([&]{return fresh.action->Snapshot().terminal;});
    c.Stop();cc.metadata.runtime_pids={children[0]};cc.metadata.remote_actions={names[0]};cc.metadata.groups={"robots"};assert(!c.Start(cc));
    Until([&]{auto p=Peer(b,30);return p.metadata_ready&&p.endpoints.size()==2;});
    for(auto scope:{n::TargetScope::GROUP,n::TargetScope::ALL}){
        n::RemoteTarget target;target.scope=scope;if(scope==n::TargetScope::GROUP)target.group="robots";
        auto fanout=Result(start(target,6));assert(!fanout.error&&fanout.endpoints.size()==2);unsigned accepted=0,rejected=0;
        for(const auto& e:fanout.endpoints){if(!e.error){++accepted;Until([&]{return e.action->Snapshot().terminal;});assert(Count(e.action->Snapshot().result)==12);}else{++rejected;assert(e.error==-EBUSY);}}
        assert(accepted==1&&rejected==1&&fanout.endpoints[0].action->Snapshot().goal!=fanout.endpoints[1].action->Snapshot().goal);
    }
    auto invalid_owner=entry;++invalid_owner.metadata.identity.runtime.process_start_ticks;assert(n::ValidateLocalActionEndpoint(invalid_owner));
    auto invalid_registration=entry;invalid_registration.action.registration_id+=100;assert(n::ValidateLocalActionEndpoint(invalid_registration));
    assert(!n::ValidateLocalActionEndpoint(entry));assert(!kill(children[0],SIGUSR1));Until([&]{return n::ValidateLocalActionEndpoint(entry)!=0;});auto stale=Result(start(Endpoint(endpoints[0]),1));assert(stale.error||stale.endpoints[0].error);
    for(auto child:children){kill(child,SIGTERM);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
    a.Stop();Until([&]{return Peer(b,10).state==n::PeerState::LOST;});assert(Result(start(Host(10),1)).error==-ENOTCONN);b.Stop();c.Stop();supervisor.End();
    std::cout<<"auto metadata/allowlist/types/owner, lifecycle/cancel, five targets, feedback, timeout, disconnect no-cancel/reconnect PASS\n";
}
void Gates(){auto port=Port();n::ControlNode client;assert(!client.Start(Config(10,port)));n::PeerDiscovery missing,incompatible;
    n::NetworkEndpointMetadata e;e.identity={{Id(30),Id(42),123,1},1ull<<62};e.kind=n::EndpointKind::ACTION;e.name="/calc";e.element_name="test";e.scope=n::NetworkScope::REMOTE;e.request_type=Type();e.response_type=Type();e.feedback_type=Type();
    auto c=Config(30,port).discovery;c.profile.capabilities=n::KNOWN_CAPABILITIES&~n::CapabilityBit(n::Capability::REMOTE_ACTION);assert(!missing.Start(c,{e}));
    c.host=Id(40);c.profile.network.major=2;c.profile.capabilities=n::KNOWN_CAPABILITIES;e.identity.runtime.host=Id(40);assert(!incompatible.Start(c,{e}));
    Until([&]{return Peer(client,30).metadata_ready&&Peer(client,40).metadata_ready;});
    auto r=One(client.StartAction(Host(30),"/calc",Type(),Type(),Type(),Bytes(2)));assert(r.reason==n::CompatibilityReason::CAPABILITY_MISSING&&!r.execution_unknown);
    r=One(client.StartAction(Host(40),"/calc",Type(),Type(),Type(),Bytes(2)));assert(r.reason==n::CompatibilityReason::NETWORK_MAJOR_MISMATCH);
    assert(!client.GetStats().requests_sent);client.Stop();missing.Stop();incompatible.Stop();std::cout<<"missing Action capability and incompatible peer denied before send PASS\n";
}
void Send(int fd,const n::ControlFrame& f){std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(f,bytes));std::size_t offset=0;while(offset<bytes.size()){auto count=send(fd,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL);assert(count>0);offset+=count;}}
n::ControlFrame Receive(int fd){std::vector<std::uint8_t> bytes;std::size_t size=n::CONTROL_PREFIX_SIZE;while(bytes.size()<size){pollfd p{fd,POLLIN,0};assert(poll(&p,1,5000)>0);std::uint8_t data[2048];auto count=recv(fd,data,std::min(sizeof(data),size-bytes.size()),0);assert(count>0);bytes.insert(bytes.end(),data,data+count);if(bytes.size()==n::CONTROL_PREFIX_SIZE)assert(!n::ControlFrameSize(bytes.data(),bytes.size(),size));}n::ControlFrame f;assert(!n::DecodeControlFrame(bytes.data(),bytes.size(),f));return f;}
void Correlation(){auto port=Port();int listener=-1;auto tcp=Port(SOCK_STREAM,&listener);assert(!listen(listener,4));
    n::PeerDiscovery server;n::ControlNode client;auto config=Config(80,port).discovery;config.metadata_port=tcp;config.profile.capabilities=n::KNOWN_CAPABILITIES;
    n::NetworkEndpointMetadata e;e.identity={{Id(80),Id(42),123,1},1ull<<62};e.kind=n::EndpointKind::ACTION;e.name="/mock";e.element_name="mock";e.scope=n::NetworkScope::REMOTE;e.request_type=Type();e.feedback_type=Type();e.response_type=Type();assert(!server.Start(config,{e}));assert(!client.Start(Config(10,port)));
    std::thread worker([&]{
        auto accept_peer=[&]{pollfd p{listener,POLLIN,0};assert(poll(&p,1,5000)>0);int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);assert(fd>=0);auto hello=Receive(fd);assert(hello.operation==n::ControlMessage::HELLO);hello.source=e.identity.runtime;hello.session_id=server.GetSessionId();Send(fd,hello);return fd;};
        auto response=[&](n::ControlFrame f,n::ControlMessage op){f.source=e.identity.runtime;f.session_id=server.GetSessionId();f.operation=op;f.timeout_ms=0;f.payload.clear();f.action_state=kcf::ActionState::ACCEPTED;return f;};
        auto terminal=[&](n::ControlFrame f){f=response(f,n::ControlMessage::ACTION_RESULT);f.request_id=0;f.action_state=kcf::ActionState::SUCCEEDED;f.payload=Bytes(42);return f;};
        int fd=accept_peer();auto goal=Receive(fd);assert(goal.operation==n::ControlMessage::ACTION_GOAL_REQUEST);Send(fd,response(goal,n::ControlMessage::ACTION_GOAL_RESPONSE));
        auto feedback=response(goal,n::ControlMessage::ACTION_FEEDBACK);feedback.request_id=0;feedback.action_state=kcf::ActionState::RUNNING;feedback.feedback_sequence=1;feedback.payload=Bytes(1);Send(fd,feedback);Send(fd,feedback);
        Send(fd,terminal(goal));Send(fd,feedback);Send(fd,terminal(goal)); // duplicate/late events cannot revive a completed session
        auto second=Receive(fd);Send(fd,response(second,n::ControlMessage::ACTION_GOAL_RESPONSE));auto cancel=Receive(fd);assert(cancel.operation==n::ControlMessage::ACTION_CANCEL_REQUEST&&cancel.goal==second.goal&&cancel.request_id!=second.request_id);
        std::this_thread::sleep_for(150ms);Send(fd,response(cancel,n::ControlMessage::ACTION_CANCEL_RESPONSE));Send(fd,terminal(second)); // late acknowledgement does not overwrite cancel_unknown
        auto third=Receive(fd);Send(fd,response(third,n::ControlMessage::ACTION_GOAL_RESPONSE));
        auto broken=terminal(third);std::vector<std::uint8_t> bytes;assert(!n::EncodeControlFrame(broken,bytes));assert(send(fd,bytes.data(),20,MSG_NOSIGNAL)==20);close(fd);
        fd=accept_peer();auto fourth=Receive(fd);assert(fourth.goal!=third.goal);Send(fd,response(fourth,n::ControlMessage::ACTION_GOAL_RESPONSE));Send(fd,terminal(fourth));
        auto fifth=Receive(fd);Send(fd,response(fifth,n::ControlMessage::ACTION_GOAL_RESPONSE));auto bad=response(fifth,n::ControlMessage::ACTION_FEEDBACK);bad.request_id=0;bad.action_state=kcf::ActionState::RUNNING;bad.feedback_sequence=1;bad.payload=Bytes(1);assert(!n::EncodeControlFrame(bad,bytes));bytes[282]=1;assert(send(fd,bytes.data(),bytes.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(bytes.size()));std::this_thread::sleep_for(50ms);close(fd);
    });
    Until([&]{return Peer(client,80).metadata_ready;});auto start=[&]{return One(client.StartAction(Host(80),"/mock",Type(),Type(),Type(),Bytes(1),2s));};
    auto first=start();assert(!first.error);Until([&]{return first.action->Snapshot().terminal;});assert(Count(first.action->Snapshot().result)==42);
    auto second=start();assert(!second.error);auto cancel=One(client.CancelAction(second.action,50ms));assert(cancel.error==-ETIMEDOUT&&cancel.cancel_unknown&&!cancel.execution_unknown);Until([&]{return second.action->Snapshot().terminal;});assert(second.action->Snapshot().cancel_unknown&&!second.action->Snapshot().cancel_accepted);
    auto third=start();assert(!third.error);Until([&]{return third.action->Snapshot().communication_lost;});assert(!third.action->Snapshot().terminal);
    auto fourth=start();assert(!fourth.error);Until([&]{return fourth.action->Snapshot().terminal;});assert(fourth.action->Snapshot().goal!=third.action->Snapshot().goal);
    auto fifth=start();assert(!fifth.error);Until([&]{return fifth.action->Snapshot().communication_lost;});assert(client.GetStats().responses_ignored>=4);
    worker.join();client.Stop();server.Stop();close(listener);std::cout<<"request/goal correlation, cancel_unknown, late events, partial/malformed frame, reconnect isolation PASS\n";
}
int main(int argc,char** argv){if(argc==3&&std::string(argv[1])=="--owner")return Owner(std::stoi(argv[2]));std::cout.setf(std::ios::unitbuf);alarm(90);Codec();Gates();Integration(argv[0]);Correlation();std::cout<<"Remote Action PASS\n";}
