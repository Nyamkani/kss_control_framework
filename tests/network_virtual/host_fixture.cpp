// Virtual LAN fixture only: real Local APIs, no SHM interception or transport mocks.
#include "kcf/network/control_node.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/action/action_server.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
namespace n=kcf::network;
static_assert(n::DISCOVERY_VERSION==2);
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
using Fields=std::map<std::string,long long>;
void Emit(Fields values){std::cout<<"{";bool first=true;for(const auto& [key,value]:values){if(!first)std::cout<<",";first=false;std::cout<<"\""<<key<<"\":";if(key.find("session")!=std::string::npos)std::cout<<static_cast<std::uint64_t>(value);else std::cout<<value;}std::cout<<"}"<<std::endl;}
void Check(int code){if(code)throw std::runtime_error("KCF error "+std::to_string(code));}
n::HostIdentity Id(unsigned char x){n::HostIdentity h;h.bytes[0]=x;return h;}
auto Descriptor(){return kcf::TypeDescriptorTraits<Value>::Get();}
n::TypeIdentity Type(){n::TypeIdentity t;Check(n::ParameterWireType(Descriptor(),t));return t;}
std::vector<std::uint8_t> Bytes(int count){Value v{count,false,1.5};kcf::DynamicPayload p;p.type_id=Type().type_id;p.bytes.resize(sizeof(v));std::memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> out;Check(n::EncodeParameterValue(Descriptor(),p,out));return out;}
int Count(const std::vector<std::uint8_t>& bytes){kcf::DynamicPayload p;Check(n::DecodeParameterValue(Descriptor(),bytes,p));Value v{};std::memcpy(&v,p.bytes.data(),sizeof(v));return v.count;}
std::string Prefix(int host){return host==1?"/virtual/a":"/virtual/b";}
void Begin(){kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);}
void End(){kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);}
template<class F>void Until(F f){auto end=Clock::now()+12s;while(!f()){if(Clock::now()>end)throw std::runtime_error("functional wait exceeded 12 seconds");std::this_thread::sleep_for(10ms);}}
int Owner(int host){
    Begin();const auto prefix=Prefix(host);kcf::Publisher<Value> publisher;Check(publisher.Create(prefix+"/topic",8));
    kcf::Parameter<Value> parameter,local;Check(parameter.Create(prefix+"/parameter",{10*host,false,1.5}));Check(local.Create(prefix+"/private",{42,false,1.5}));
    kcf::ServiceServer service;Check(service.Create(39001));std::atomic<int> service_calls{0},goals{0},goal_value{0};
    Check(service.Register<Value,Value>(9,prefix+"/service",[&](const Value& input,Value& out){++service_calls;if(input.count==99)std::this_thread::sleep_for(600ms);out=input;out.count*=2;}));
    kcf::ActionServer<Value,Value,Value> action;Check(action.Create(service,{1,2,3},prefix+"/action",[&](std::uint64_t,const Value& v){++goals;if(v.count==99)std::this_thread::sleep_for(600ms);goal_value=v.count;return 0;}));Check(service.Start());
    std::atomic<bool> running{true},publishing{true};std::atomic<int> published{0},worker_error{0};
    std::thread worker([&]{try{auto next=Clock::now(),feedback=next,start=next,cancel=Clock::time_point{};std::uint64_t tracked=0;
        while(running){if(publishing){int count=published+1;int code=publisher.Publish({count,false,1.5});if(!code)published=count;else if(code!=-EAGAIN)Check(code);}
            auto state=action.GetState();auto id=action.GetGoalId();
            if(id!=tracked){tracked=id;cancel={};}
            if((state==kcf::ActionState::ACCEPTED||state==kcf::ActionState::RUNNING)&&action.IsCancelRequested(id)){
                if(cancel==Clock::time_point{})cancel=Clock::now()+250ms;
                if(Clock::now()>=cancel)Check(action.Canceled(id,{-1,false,1.5}));
            }else if(state==kcf::ActionState::ACCEPTED){
                int code=action.Start(id); // Cancel may race between the check and Start.
                if(code!=-EINVAL||!action.IsCancelRequested(id))Check(code);
                start=feedback=Clock::now();
            }else if(state==kcf::ActionState::RUNNING){
                if(Clock::now()-start>=1200ms)Check(action.Succeed(id,{goal_value.load()*2,false,1.5}));
                else if(Clock::now()>=feedback){Check(action.PublishFeedback(id,{published.load(),false,1.5}));feedback=Clock::now()+25ms;}
            }
            next+=5ms;if(next<Clock::now())next=Clock::now()+5ms;std::this_thread::sleep_until(next);
        }}catch(const std::exception& e){std::cerr<<"owner worker: "<<e.what()<<std::endl;worker_error=-EFAULT;}});
    Emit({{"ready",1},{"pid",getpid()}});std::string command;
    while(std::getline(std::cin,command)&&command!="QUIT"){
        if(command=="ISOLATION"){
            kcf::Parameter<Value> other_parameter;kcf::SharedChannel<Value> other_topic;
            Emit({{"parameter_open",other_parameter.Open(Prefix(3-host)+"/parameter")},{"topic_open",other_topic.Open(Prefix(3-host)+"/topic")}});continue;
        }
        if(command=="PAUSE")publishing=false;
        if(command=="RESUME")publishing=true;
        Value v{};int code=parameter.Get(v);kcf::SharedChannel<Value> reader;std::uint64_t sequence=0;Value snapshot{};int read=reader.Open(prefix+"/topic");if(!read)read=reader.ReadLatestSnapshot(snapshot,sequence);
        Emit({{"code",code},{"worker_error",worker_error},{"published",published},{"parameter",v.count},{"service_calls",service_calls},{"goals",goals},{"local_read",read},{"local_sequence",static_cast<long long>(sequence)},{"local_value",snapshot.count}});
    }
    running=false;worker.join();service.Stop();action.Unlink();action.Close();publisher.Unlink();publisher.Close();parameter.Unlink();parameter.Close();local.Unlink();local.Close();End();return 0;
}
int Subscriber(int host){Begin();kcf::Subscriber<Value> subscriber;std::atomic<int> latest{0},bad{0};std::atomic<long long> count{0};const auto name=Prefix(3-host)+"/topic";
    int code=subscriber.Create(name,[&](const Value& v){if(v.count<=latest||v.enabled||v.gain!=1.5)++bad;latest=v.count;++count;});Emit({{"ready",!code},{"code",code},{"pid",getpid()}});if(code){End();return 2;}
    std::string command;while(std::getline(std::cin,command)&&command!="QUIT"){
        kcf::SharedChannel<Value> reader;std::uint64_t sequence=0;Value value{};int read=reader.Open(name);if(!read)read=reader.ReadLatestSnapshot(value,sequence);
        Emit({{"callbacks",count},{"latest",latest},{"bad",bad},{"read",read},{"sequence",static_cast<long long>(sequence)},{"value",value.count}});
    }subscriber.Close();End();return 0;
}
n::ParameterEndpointResult Result(std::future<n::ParameterBatchResult> future){if(future.wait_for(8s)!=std::future_status::ready)throw std::runtime_error("future not completed");auto batch=future.get();if(batch.error){n::ParameterEndpointResult r;r.error=batch.error;return r;}if(batch.endpoints.size()!=1)throw std::runtime_error("expected one endpoint");return batch.endpoints[0];}
int Gateway(int host){n::ControlNode node;n::ControlConfig c;c.discovery.host=Id(host);c.discovery.boot_id=Id(80+host);c.discovery.interface_address=host==1?"10.203.0.2":"10.203.0.3";
    c.discovery.announce_interval=100ms;c.discovery.peer_timeout=1200ms;c.discovery.metadata_retry=100ms;c.metadata_interval=50ms;c.connect_timeout=2s;c.reconnect_interval=100ms;c.topics.enabled=true;
    c.metadata.remote_topics={"/virtual/a/topic","/virtual/b/topic"};c.metadata.remote_parameters={Prefix(host)+"/parameter"};c.metadata.remote_services={Prefix(host)+"/service"};c.metadata.remote_actions={Prefix(host)+"/action"};
    c.topics.local_types={{"/virtual/a/topic",Descriptor()},{"/virtual/b/topic",Descriptor()}};Check(node.Start(c));
    n::RemoteTarget target;target.scope=n::TargetScope::HOST;target.host=Id(3-host);auto remote=Prefix(3-host);Emit({{"ready",1},{"pid",getpid()}});
    std::string line;while(std::getline(std::cin,line)&&line!="QUIT"){
        std::istringstream input(line);std::string op;int arg=0;input>>op>>arg;Fields out;
        if(op=="STATUS"){
            int peers=0,online=0,self=0,metadata=0,compatible=0,topics=0,parameters=0,services=0,actions=0,virtual_ip=0;std::uint64_t session=0,caps=0;
            for(const auto& p:node.GetPeers()){++peers;if(p.host==Id(host))++self;if(p.host!=Id(3-host))continue;virtual_ip=p.address==(host==1?"10.203.0.3":"10.203.0.2");session=p.session_id;caps=p.profile.capabilities;online=p.state==n::PeerState::ONLINE;metadata=p.metadata_ready;compatible=p.compatibility.Compatible();for(const auto& e:p.endpoints){topics+=e.kind==n::EndpointKind::TOPIC;parameters+=e.kind==n::EndpointKind::PARAMETER;services+=e.kind==n::EndpointKind::SERVICE;actions+=e.kind==n::EndpointKind::ACTION;}}
            out={{"virtual_ip",virtual_ip},{"peers",peers},{"online",online},{"self",self},{"metadata",metadata},{"compatible",compatible},{"peer_session",static_cast<long long>(session)},{"capabilities",static_cast<long long>(caps)},{"topic_endpoints",topics},{"parameter_endpoints",parameters},{"service_endpoints",services},{"action_endpoints",actions},{"error",node.GetLastError()},{"connections",static_cast<long long>(node.GetStats().connections)}};
            auto d=node.GetTopicDiagnostics();for(const auto& p:d.proxies){out["proxy_recovered"]+=p.recovery==n::ProxyRecoveryState::RECOVERED;out["proxy_stale_detected"]+=p.stale_detected;}out["send_routes"]=d.active_send_routes;out["receive_routes"]=d.active_receive_routes;out["proxies"]=d.proxies.size();out["stale"]=d.stale;
            for(const auto& r:d.routes){out["sent"]+=r.packets_sent;out["received"]+=r.packets_received;out["duplicate"]+=r.duplicate;out["out_of_order"]+=r.out_of_order;out["missed"]+=r.missed_sequence;out["dropped"]+=r.dropped;out["skipped"]+=r.local_samples_skipped;out["conflict"]|=r.error==-EEXIST;if(!r.sending&&r.state==n::TopicRouteState::ACTIVE){out["route_id"]=r.route_id;out["source_session"]=r.source_session;out["destination_session"]=r.destination_session;}}
        }else{
            n::ParameterEndpointResult r;
            if(op=="GET"||op=="PRIVATE"||op=="WRONG"){auto t=Type();if(op=="WRONG")++t.type_id;r=Result(node.Get(target,remote+(op=="PRIVATE"?"/private":"/parameter"),t,3s));if(!r.error&&!r.value.empty())out["value"]=Count(r.value);}
            else if(op=="SET")r=Result(node.Set(target,remote+"/parameter",Type(),Bytes(arg),3s));
            else if(op=="SERVICE"||op=="TIMEOUT_SERVICE"){r=Result(node.CallService(target,remote+"/service",Type(),Type(),Bytes(op=="TIMEOUT_SERVICE"?99:arg),op=="TIMEOUT_SERVICE"?150ms:3s));if(!r.error)out["value"]=Count(r.value);}
            else if(op=="ACTION"||op=="CANCEL"||op=="TIMEOUT_ACTION"){
                r=Result(node.StartAction(target,remote+"/action",Type(),Type(),Type(),Bytes(op=="TIMEOUT_ACTION"?99:arg),op=="TIMEOUT_ACTION"?150ms:3s));
                if(!r.error){auto handle=r.action;out["accepted"]=handle->Snapshot().accepted;std::uint64_t previous=0;int updates=0;
                    if(op=="CANCEL"){auto cancel=Result(node.CancelAction(handle,3s));out["cancel_code"]=cancel.error;out["cancel_accepted"]=handle->Snapshot().cancel_accepted;out["terminal_at_ack"]=handle->Snapshot().terminal;}
                    Until([&]{auto s=handle->Snapshot();if(s.feedback_sequence>previous){previous=s.feedback_sequence;++updates;}return s.terminal;});auto s=handle->Snapshot();out["terminal"]=s.terminal;out["state"]=static_cast<int>(s.state);out["succeeded"]=s.state==kcf::ActionState::SUCCEEDED;out["canceled"]=s.state==kcf::ActionState::CANCELED;out["feedback_updates"]=updates;out["terminal_error"]=s.error;if(!s.result.empty())out["value"]=Count(s.result);
                }
            }else throw std::runtime_error("unknown command");
            out["code"]=r.error;out["reason"]=static_cast<int>(r.reason);out["execution_unknown"]=r.execution_unknown;
        }Emit(out);
    }node.Stop();return 0;
}
int main(int argc,char** argv){try{if(argc!=3)return 2;int host=std::stoi(argv[2]);if(host!=1&&host!=2)return 2;std::string mode=argv[1];if(mode=="owner")return Owner(host);if(mode=="gateway")return Gateway(host);if(mode=="subscriber")return Subscriber(host);return 2;}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
