#include "gateway_support.hpp"
#include "kcf/network/remote_topic_client.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include "../../kcf/src/network/control_client_io.hpp"
using namespace std::chrono_literals;
namespace n=kcf::network;
static gateway::Options Parse(std::vector<std::string> args){std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());return gateway::Parse(argv.size(),argv.data());}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(20ms);}}
int main(int argc,char** argv){
    assert(!n::ControlConfig{}.topic_observer_enabled);
    auto off=Parse({"gateway"});n::NetworkInterfaceSelection nic{"lo","127.0.0.1",1,false};assert(!gateway::Config(off,nic).topics.enabled&&!gateway::Config(off,nic).topic_observer_enabled);
    auto options=Parse({"gateway","--topic-allow-prefix","/policy","--topic-allow-prefix","/extra/","--topic-exclude-prefix","/policy/private"});
    auto config=gateway::Config(options,nic);assert(config.topics.enabled&&config.topics.max_hz==50&&config.metadata.remote_topics.empty());
    assert(config.parameter_set_clients.empty()&&config.service_call_clients.empty()&&config.action_goal_clients.empty()&&config.action_cancel_clients.empty()&&config.descriptor_query_clients.empty());
    for(auto args:std::vector<std::vector<std::string>>{{"x","--topic-allow-prefix","bad"},{"x","--topic-allow-prefix","/a","--topic-allow-prefix","/a/"},{"x","--topic-exclude-prefix"}}){bool bad=false;try{Parse(args);}catch(...){bad=true;}assert(bad);}
    auto excluded=Parse({"x","--topic-exclude-prefix","/policy"});assert(!gateway::Config(excluded,nic).topics.enabled&&!gateway::Config(excluded,nic).topic_observer_enabled);
    std::cout<<"Gateway repeated Prefix CLI/validation/default deny/rate/Config PASS\n";
    if(argc==1)return 0;
    assert(argc==2&&std::string(argv[1])=="--network");
    config.discovery.host.bytes[0]=10;config.discovery.boot_id.bytes[0]=42;config.discovery.port=48651;
    config.discovery.metadata_port=49011;config.topics.port=48652;
    config.discovery.announce_interval=60ms;config.discovery.peer_timeout=400ms;config.discovery.metadata_retry=40ms;
    config.metadata.runtime_pids={getpid()};
    n::HostIdentity observer_id;observer_id.bytes[0]=20;assert(config.topic_observer_enabled);
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlNode sender;assert(sender.Start(config)==0);
    n::DiscoveryConfig dc=config.discovery;dc.host=observer_id;dc.metadata_port=0;
    dc.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::REMOTE_TOPIC_RECEIVER)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
    n::PeerDiscovery discovery;assert(discovery.Start(dc)==0);
    n::RemoteControlClient control(discovery,dc);assert(control.Start()==0);
    n::RemoteTopicClient client(discovery,dc,control);n::RemoteTopicConfig tc;tc.publisher_port=config.topics.port;assert(client.Start(tc)==0);
    const auto peer=[&]{for(const auto& p:discovery.GetPeers())if(p.host==config.discovery.host)return p;return n::PeerInfo{};};
    Until([&]{return peer().metadata_ready;});assert(peer().endpoints.empty());
    kcf::Publisher<Value> publisher,private_publisher;assert(publisher.Create("/policy/value")==0);assert(private_publisher.Create("/policy/private/value")==0);
    kcf::Subscriber<Value> local_sub;assert(local_sub.Create("/policy/value",[](const Value&){})==0);
    n::NetworkEndpointMetadata endpoint;
    Until([&]{auto p=peer();if(!p.metadata_ready||p.endpoints.size()!=3)return false;
        for(const auto& e:p.endpoints){if(e.role==n::EndpointRole::CONSUMER||e.name=="/policy/private/value")assert(e.scope==n::NetworkScope::LOCAL);else{assert(e.scope==n::NetworkScope::REMOTE);endpoint=e;}}return true;});
    kcf::TypeDescriptor schema;
    for(const auto& e:peer().endpoints)if(e.scope==n::NetworkScope::LOCAL)
        assert(control.QueryTypeDescriptor(e.identity,peer().session_id,e.name,n::DescriptorRole::VALUE,e.value_type,schema)==-EACCES);
    assert(control.QueryTypeDescriptor(endpoint.identity,peer().session_id+1,endpoint.name,n::DescriptorRole::VALUE,endpoint.value_type,schema)==-ESTALE);
    auto wrong=endpoint.value_type;++wrong.schema_id;
    assert(control.QueryTypeDescriptor(endpoint.identity,peer().session_id,endpoint.name,n::DescriptorRole::VALUE,wrong,schema)==-EPROTOTYPE);
    for(int i=0;i<30;++i){assert(publisher.Publish({i,true,1.5})==0);std::this_thread::sleep_for(5ms);}
    assert(sender.GetTopicDiagnostics().active_send_routes==0);
    for(const auto& r:sender.GetTopicDiagnostics().routes)assert(r.packets_sent==0);
    std::shared_ptr<n::RemoteTopicSubscription> handle;
    assert(client.Subscribe(endpoint.identity,peer().session_id,endpoint.name,endpoint.value_type,handle,2s)==0);
    Until([&]{assert(publisher.Publish({123,true,1.5})==0);return handle->Snapshot().has_value;});
    auto snapshot=handle->Snapshot();kcf::DynamicPayload decoded;
    assert(n::DecodeParameterValue(kcf::TypeDescriptorTraits<Value>::Get(),snapshot.payload,decoded)==0);
    Value value{};std::memcpy(&value,decoded.bytes.data(),sizeof(value));assert(value.count==123&&value.enabled&&value.gain==1.5);
    Until([&]{return sender.GetTopicDiagnostics().active_send_routes==1;});
    assert(client.Unsubscribe(handle)==0);
    // Raw requests bypass client metadata preflight, checking the Gateway itself.
    {
        using namespace n::client_io;
        Socket socket;n::ControlFrame hello,reply;
        hello.source={dc.host,dc.boot_id,getpid(),0};
        assert(kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks));
        hello.session_id=discovery.GetSessionId();
        Until([&]{for(const auto& p:sender.GetPeers())if(p.host==dc.host)return p.state==n::PeerState::ONLINE;return false;});
        assert(!ConnectHello(socket,dc,peer(),hello,reply,Clock::now()+2s,nullptr));
        auto request=hello;request.operation=n::ControlMessage::DESCRIPTOR_REQUEST;
        request.descriptor_role=n::DescriptorRole::VALUE;request.timeout_ms=1000;request.request_id=900000;
        auto raw=[&](const n::NetworkEndpointMetadata& e){request.endpoint=e.identity;request.name=e.name;request.type=e.value_type;++request.request_id;
            assert(!SendFrame(socket.fd,request,Clock::now()+2s));
            assert(!ReceiveFrame(socket.fd,n::ControlMessage::DESCRIPTOR_RESPONSE,reply,Clock::now()+2s));return reply.status;};
        assert(raw(endpoint)==0);
        for(const auto& e:peer().endpoints)if(e.scope==n::NetworkScope::LOCAL)assert(raw(e)==-EACCES);
        request.descriptor_role=n::DescriptorRole::FEEDBACK;assert(raw(endpoint)==-EACCES);
    }

    // Restart clears earlier short-lived connections; exercise the shared cap
    // with two unrelated Observer identities and verify cleanup by subscribing again.
    auto old_session=peer().session_id;sender.Stop();assert(sender.Start(config)==0);
    Until([&]{return peer().metadata_ready&&peer().session_id!=old_session&&peer().endpoints.size()==3;});
    {
        using namespace n::client_io;
        auto dc2=dc;dc2.host.bytes[0]=21;n::PeerDiscovery discovery2;assert(!discovery2.Start(dc2));
        Until([&]{int ready=0;for(const auto& p:sender.GetPeers())if((p.host==dc.host||p.host==dc2.host)&&p.state==n::PeerState::ONLINE)++ready;return ready==2;});
        std::vector<std::unique_ptr<Socket>> sockets;
        for(int i=0;i<9;++i){auto socket=std::make_unique<Socket>();n::ControlFrame hello,reply;
            const auto& cfg=i%2?dc2:dc;hello.source={cfg.host,cfg.boot_id,getpid(),0};
            assert(kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks));
            hello.session_id=i%2?discovery2.GetSessionId():discovery.GetSessionId();
            const int code=ConnectHello(*socket,cfg,peer(),hello,reply,Clock::now()+2s,nullptr);
            if(i==8){assert(code!=0);break;}assert(code==0);
            auto request=hello;request.operation=n::ControlMessage::DESCRIPTOR_REQUEST;request.request_id=100+i;
            request.endpoint=endpoint.identity;request.name=endpoint.name;request.type=endpoint.value_type;
            request.descriptor_role=n::DescriptorRole::VALUE;request.timeout_ms=1000;
            assert(!SendFrame(socket->fd,request,Clock::now()+2s));
            assert(!ReceiveFrame(socket->fd,n::ControlMessage::DESCRIPTOR_RESPONSE,reply,Clock::now()+2s)&&reply.status==0);
            sockets.push_back(std::move(socket));
        }
        sockets.clear();discovery2.Stop();
    }
    std::cout<<"Two unregistered Observers / 8 shared connections / ninth rejection / socket release PASS\n";
    // OFF with an unrelated explicit grant still advertises the operation, but
    // this Observer must be denied by the server (not merely absent capability).
    old_session=peer().session_id;sender.Stop();auto denied_config=config;
    denied_config.topic_observer_enabled=false;n::HostIdentity other;other.bytes[0]=99;
    denied_config.descriptor_query_clients={other};assert(!sender.Start(denied_config));
    Until([&]{return peer().metadata_ready&&peer().session_id!=old_session&&peer().endpoints.size()==3;});
    Until([&]{for(const auto& p:sender.GetPeers())if(p.host==dc.host)return p.state==n::PeerState::ONLINE;return false;});
    assert(control.QueryTypeDescriptor(endpoint.identity,peer().session_id,endpoint.name,n::DescriptorRole::VALUE,endpoint.value_type,schema)!=0);
    const auto session=peer().session_id;sender.Stop();
    // Same Config reconstruction as main's rebind path, policy retained.
    auto rebound=gateway::Config(options,nic);rebound.discovery=config.discovery;rebound.topics.port=config.topics.port;
    rebound.metadata.runtime_pids={getpid()};assert(rebound.topic_observer_enabled);
    assert(sender.Start(rebound)==0);
    Until([&]{return peer().metadata_ready&&peer().session_id!=session&&peer().endpoints.size()==3;});
    for(const auto& e:peer().endpoints)if(e.name=="/policy/value"&&e.role==n::EndpointRole::PROVIDER)assert(e.scope==n::NetworkScope::REMOTE);
    assert(local_sub.Close()==0);assert(publisher.Close()==0);assert(publisher.Unlink()==0);
    assert(private_publisher.Close()==0);assert(private_publisher.Unlink()==0);
    Until([&]{return peer().metadata_ready&&peer().endpoints.empty();});
    assert(control.QueryTypeDescriptor(endpoint.identity,peer().session_id,endpoint.name,n::DescriptorRole::VALUE,endpoint.value_type,schema)!=0);
    client.Stop();control.Stop();discovery.Stop();sender.Stop();
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Gateway policy Metadata dynamic add/remove/Subscriber exclusion/no-interest no-send/actual DATA/rebind policy PASS (unregistered read-only Topic Observer)\n";
}
