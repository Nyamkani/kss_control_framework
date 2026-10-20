#include "kcf/network/remote_topic_client.hpp"
#include "kcf/network/network_diagnostics_client.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/action/action_server.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <filesystem>
using namespace std::chrono_literals;
namespace n=kcf::network;
// Test-only per-process Topic namespaces; introspection remains real/shared.
static std::string prefix;
static std::string Name(const char* name){return !prefix.empty()&&std::strncmp(name,"/kcf_generic_",13)==0?prefix+(name+1):name;}
extern "C" int shm_open(const char* name,int flags,mode_t mode){static auto fn=reinterpret_cast<int(*)(const char*,int,mode_t)>(dlsym(RTLD_NEXT,"shm_open"));return fn(Name(name).c_str(),flags,mode);}
extern "C" int shm_unlink(const char* name){static auto fn=reinterpret_cast<int(*)(const char*)>(dlsym(RTLD_NEXT,"shm_unlink"));return fn(Name(name).c_str());}
struct Sample{std::int32_t host;std::uint32_t count;bool enabled;double axes[3];};
namespace kcf {template<> struct TypeDescriptorTraits<Sample>{static constexpr bool defined=true;static TypeDescriptor Get(){auto d=MakeTypeDescriptor<Sample>("test.generic.sample.v1");d.field_count=4;
 d.fields[0]=MakeField<Sample,std::int32_t>("host",offsetof(Sample,host));d.fields[1]=MakeField<Sample,std::uint32_t>("count",offsetof(Sample,count));d.fields[2]=MakeField<Sample,bool>("enabled",offsetof(Sample,enabled));d.fields[3]=MakeField<Sample,double[3]>("axes",offsetof(Sample,axes));return d;}};}
n::HostIdentity Id(unsigned h){n::HostIdentity id;id.bytes[0]=h;return id;}
std::string IP(unsigned h){return "127.0.0."+std::to_string(h);}
std::uint16_t Port(){int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t size=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size));close(fd);return ntohs(a.sin_port);}
template<class F>void Until(F f){auto end=std::chrono::steady_clock::now()+8s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
n::DiscoveryConfig Discovery(unsigned host,std::uint16_t port){n::DiscoveryConfig c;c.host=Id(host);c.boot_id=Id(42);c.interface_address=IP(host);c.port=port;c.announce_interval=50ms;c.peer_timeout=350ms;c.metadata_retry=30ms;return c;}
n::ControlConfig Gateway(unsigned host,std::uint16_t discovery,std::uint16_t data){n::ControlConfig c;c.discovery=Discovery(host,discovery);c.metadata.runtime_pids={getpid()};c.metadata_interval=30ms;c.topics.enabled=true;c.topics.port=data;c.diagnostics_enabled=true;c.descriptor_query_clients={Id(90)};c.parameter_get_clients=c.parameter_set_clients=c.service_call_clients=c.action_goal_clients=c.action_cancel_clients={Id(90)};return c;}
std::vector<std::uint8_t> Bytes(int count){auto d=kcf::TypeDescriptorTraits<Value>::Get();Value v{count,true,1.5};kcf::DynamicPayload p;p.type_id=d.type_id;p.bytes.resize(sizeof(v));memcpy(p.bytes.data(),&v,sizeof(v));std::vector<std::uint8_t> out;assert(!n::EncodeParameterValue(d,p,out));return out;}
int Owner(unsigned host,std::uint16_t discovery,std::uint16_t data,int pipe){
 prefix="/generic_host_"+std::to_string(host)+"_";kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
 kcf::Publisher<Sample> publisher,second;assert(!publisher.Create("/kcf_generic_same"));if(host==10)assert(!second.Create("/kcf_generic_second"));
 kcf::Parameter<Value> parameter;auto pn="/parameter_"+std::to_string(host);assert(!parameter.Create(pn,{1,true,1.5}));
 kcf::ServiceServer service;assert(!service.Create(Port()));assert((!service.Register<Value,Value>(4,"/service",[](const Value& in,Value& out){out=in;out.count*=2;})));
 kcf::ActionServer<Value,Value,Value> action;std::atomic<int> goal{0};assert(!action.Create(service,{1,2,3},"/kcf_generic_action",[&](std::uint64_t,const Value& v){goal=v.count;return 0;}));assert(!service.Start());
 kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);n::ControlNode node;auto config=Gateway(host,discovery,data);config.metadata.remote_topics={"/kcf_generic_same"};if(host==10)config.metadata.remote_topics.push_back("/kcf_generic_second");config.metadata.remote_parameters={pn};config.metadata.remote_services={"/service"};config.metadata.remote_actions={"/kcf_generic_action"};assert(!node.Start(config));
 std::uint32_t count=0;unsigned ticks=0;bool run=true;
 while(run){Sample sample{static_cast<int>(host),++count,true,{double(host),2,3}};int code=publisher.Publish(sample);assert(!code||code==-EAGAIN);if(host==10){code=second.Publish(sample);assert(!code||code==-EAGAIN);}
 auto state=action.GetState();auto id=action.GetGoalId();if((state==kcf::ActionState::RUNNING||state==kcf::ActionState::ACCEPTED)&&action.IsCancelRequested(id))assert(!action.Canceled(id,{-1,true,1.5}));
 else if(state==kcf::ActionState::ACCEPTED){code=action.Start(id);assert(!code||code==-EINVAL);ticks=0;}
 else if(state==kcf::ActionState::RUNNING){code=action.PublishFeedback(id,{int(++ticks),true,1.5});assert(!code||code==-EAGAIN);if(ticks>=200)assert(!action.Succeed(id,{goal.load()*2,true,1.5}));}
 pollfd p{pipe,POLLIN,0};assert(poll(&p,1,5)>=0);if(p.revents&POLLIN){char op;assert(recv(pipe,&op,1,0)==1);int result=0;if(op=='N')node.Stop();else if(op=='R')result=node.Start(config);else if(op=='X')run=false;assert(send(pipe,&result,sizeof(result),MSG_NOSIGNAL)==sizeof(result));}if(p.revents&POLLHUP)break;
 }
 node.Stop();service.Stop();action.Unlink();action.Close();parameter.Unlink();parameter.Close();publisher.Unlink();publisher.Close();if(host==10){second.Unlink();second.Close();}kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);return 0;
}
struct Child{pid_t pid;int fd;Child(const char* executable,unsigned host,std::uint16_t discovery,std::uint16_t data){int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));pid=fork();assert(pid>=0);if(!pid){close(pair[0]);auto h=std::to_string(host),d=std::to_string(discovery),t=std::to_string(data),p=std::to_string(pair[1]);execl(executable,executable,"owner",h.c_str(),d.c_str(),t.c_str(),p.c_str(),nullptr);_exit(127);}close(pair[1]);fd=pair[0];}void Command(char c){assert(send(fd,&c,1,MSG_NOSIGNAL)==1);pollfd p{fd,POLLIN,0};assert(poll(&p,1,5000)>0);int result;assert(recv(fd,&result,sizeof(result),0)==sizeof(result)&&!result);}~Child(){Command('X');int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));close(fd);}};
void Inject(const n::RemoteTopicSnapshot& snapshot,const n::TypeIdentity& type,bool data,const std::string& from,std::uint16_t source_port,const std::string& to,std::uint16_t port){
 n::TopicPacket p;p.kind=data?n::TopicMessage::DATA:n::TopicMessage::INTEREST;p.source=snapshot.route.source;p.destination=snapshot.route.destination;p.source_session=snapshot.route.source_session;p.destination_session=snapshot.route.destination_session;p.route_id=snapshot.route.route_id;p.name=snapshot.route.name;p.type=type;
 if(data){p.sequence=snapshot.sequence+10;p.sample_sequence=snapshot.sample_sequence+10;p.payload=snapshot.payload;}
 std::vector<std::uint8_t> bytes;assert(!n::EncodeTopicPacket(p,bytes));int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(source_port);assert(inet_pton(AF_INET,from.c_str(),&a.sin_addr)==1);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));a.sin_port=htons(port);assert(inet_pton(AF_INET,to.c_str(),&a.sin_addr)==1);assert(sendto(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&a),sizeof(a))==static_cast<ssize_t>(bytes.size()));close(fd);
}
std::size_t Fds(){std::size_t count=0;for(const auto& e:std::filesystem::directory_iterator("/proc/self/fd")){(void)e;++count;}return count;}
std::size_t Proxies(){std::size_t count=0;for(const auto& e:std::filesystem::directory_iterator("/dev/shm")){auto name=e.path().filename().string();if(name.find("kcf_remote_topic_")==0||name.find("kcf_network_proxy_")==0)++count;}return count;}
int main(int argc,char** argv){alarm(120);if(argc==6)return Owner(std::stoi(argv[2]),std::stoi(argv[3]),std::stoi(argv[4]),std::stoi(argv[5]));std::cout.setf(std::ios::unitbuf);
 auto dp=Port(),tp=Port();Child a(argv[0],10,dp,tp),b(argv[0],20,dp,tp);
 auto dc=Discovery(90,dp);dc.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::REMOTE_TOPIC_RECEIVER)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
 n::PeerDiscovery discovery;assert(!discovery.Start(dc));n::RemoteControlClient control(discovery,dc);assert(!control.Start());n::NetworkDiagnosticsClient diagnostics(discovery,dc);
 n::RemoteTopicClient topics(discovery,dc,control);n::RemoteTopicConfig tc;tc.publisher_port=tp;tc.max_subscriptions=4;assert(!topics.Start(tc)&&topics.ReceivePort()!=tp);
 auto peer=[&](unsigned host){n::PeerInfo out;Until([&]{for(auto& p:discovery.GetPeers())if(p.host==Id(host)&&p.metadata_ready&&p.endpoints.size()>=(host==10?5u:4u)&&p.state==n::PeerState::ONLINE){out=p;return true;}return false;});return out;};
 auto endpoint=[](const n::PeerInfo& p,const std::string& name,n::EndpointKind kind){for(auto& e:p.endpoints)if(e.name==name&&e.kind==kind&&e.role==n::EndpointRole::PROVIDER)return e;assert(false);return n::NetworkEndpointMetadata{};};
 auto pa=peer(10),pb=peer(20);auto ea=endpoint(pa,"/kcf_generic_same",n::EndpointKind::TOPIC),eb=endpoint(pb,ea.name,n::EndpointKind::TOPIC),e2=endpoint(pa,"/kcf_generic_second",n::EndpointKind::TOPIC);
 using Handle=std::shared_ptr<n::RemoteTopicSubscription>;Handle ha,hb,ha2,h2,extra;
 auto subscribe=[&](const auto& e,std::uint64_t session,Handle& h){int code=topics.Subscribe(e.identity,session,e.name,e.value_type,h,2s);if(code)std::cerr<<"Subscribe "<<e.name<<" host="<<unsigned(e.identity.runtime.host.bytes[0])<<" error="<<code<<"\n";return code;};assert(!subscribe(ea,pa.session_id,ha));assert(!subscribe(eb,pb.session_id,hb));assert(!subscribe(ea,pa.session_id,ha2));assert(!subscribe(e2,pa.session_id,h2));assert(subscribe(ea,pa.session_id,extra)==-ENOSPC);
 auto value=[&](Handle h,int host){auto s=h->Snapshot();if(!s.has_value)return false;assert(s.state==n::RemoteTopicState::ACTIVE);kcf::DynamicPayload v;assert(!n::DecodeParameterValue(s.descriptor,s.payload,v));int got=0;double axis=0;memcpy(&got,v.bytes.data()+s.descriptor.fields[0].offset,sizeof(got));memcpy(&axis,v.bytes.data()+s.descriptor.fields[3].offset,sizeof(axis));assert(got==host&&axis==host&&s.descriptor.fields[3].array_count==3);return true;};
 Until([&]{return value(ha,10)&&value(hb,20)&&value(ha2,10)&&value(h2,10);});assert(ha->Snapshot().proxy_name!=hb->Snapshot().proxy_name&&ha->Snapshot().route.destination!=ha2->Snapshot().route.destination);
 auto first_sequence=ha->Snapshot().sequence;Inject(ha->Snapshot(),ea.value_type,false,dc.interface_address,0,pa.address,tp);auto forged=ha->Snapshot();++forged.route.route_id;Inject(forged,ea.value_type,false,dc.interface_address,0,pa.address,tp);Until([&]{return ha->Snapshot().sequence>first_sequence+2;});
 auto session_before=discovery.GetSessionId();assert(!discovery.UpdateMetadata({}));assert(discovery.GetSessionId()==session_before);first_sequence=ha->Snapshot().sequence;Until([&]{return ha->Snapshot().sequence>first_sequence;});
 std::cout<<"Exact same-name selection, scalar/array canonical decode, independent Consumers, limit PASS\n";
 // Legacy Gateway and Tools receive on exactly the same local IPv4, different ports.
 kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
 auto local_config=Gateway(91,dp,tp);local_config.discovery.interface_address=dc.interface_address;local_config.metadata.remote_topics={e2.name};local_config.topics.local_types={{e2.name,kcf::TypeDescriptorTraits<Sample>::Get()}};n::ControlNode local;assert(!local.Start(local_config));Until([&]{return !local.GetTopicDiagnostics().proxies.empty();});std::atomic<unsigned> received{0};kcf::Subscriber<Sample> subscriber;assert(!subscriber.Create(e2.name,[&](const Sample& s){assert(s.host==10);++received;}));Until([&]{return received>2&&value(h2,10);});
 auto parameter=endpoint(pa,"/parameter_10",n::EndpointKind::PARAMETER),service=endpoint(pa,"/service",n::EndpointKind::SERVICE),action=endpoint(pa,"/kcf_generic_action",n::EndpointKind::ACTION);
 auto goal=control.StartAction(action.identity,pa.session_id,action.name,action.request_type,action.feedback_type,action.response_type,Bytes(7),2s);assert(!goal.error);Until([&]{return goal.handle->Snapshot().feedback_sequence>0;});
 std::vector<std::uint8_t> out;assert(!control.GetParameter(parameter.identity,pa.session_id,parameter.name,parameter.value_type,out,2s));assert(!control.SetParameter(parameter.identity,pa.session_id,parameter.name,parameter.value_type,Bytes(42),2s).error);auto call=control.CallService(service.identity,pa.session_id,service.name,service.request_type,service.response_type,Bytes(6),2s);assert(!call.error&&call.value==Bytes(12));n::NetworkDiagnosticsSnapshot diagnostic;assert(!diagnostics.Query(pa.host,diagnostic,2s));auto cancel=control.CancelAction(goal.handle,2s);assert(!cancel.error&&cancel.accepted);Until([&]{return goal.handle->Snapshot().terminal;});assert(goal.handle->Snapshot().state==kcf::ActionState::CANCELED);assert(value(ha,10)&&value(hb,20));
 auto success=control.StartAction(action.identity,pa.session_id,action.name,action.request_type,action.feedback_type,action.response_type,Bytes(8),2s);assert(!success.error);Until([&]{return success.handle->Snapshot().terminal;});assert(success.handle->Snapshot().state==kcf::ActionState::SUCCEEDED&&success.handle->Snapshot().result==Bytes(16));
 std::cout<<"Same-IP Gateway + ephemeral Client, concurrent Action/Get/Set/Service/Diagnostics PASS\n";
 auto old=ha->Snapshot();assert(!topics.Unsubscribe(ha));assert(ha->Snapshot().state==n::RemoteTopicState::CLOSED);int fd=shm_open(old.proxy_name.c_str(),O_RDONLY,0);assert(fd<0&&errno==ENOENT);auto sequence=ha2->Snapshot().sequence;Until([&]{return ha2->Snapshot().sequence>sequence;});
 auto fd_baseline=Fds(),proxy_baseline=Proxies();
 for(int repeat=0;repeat<5;++repeat){assert(!subscribe(ea,pa.session_id,ha));Until([&]{return value(ha,10);});assert(!topics.Unsubscribe(ha));Until([&]{return Fds()<=fd_baseline&&Proxies()==proxy_baseline;});}
 a.Command('N');Until([&]{return ha2->Snapshot().state==n::RemoteTopicState::LOST;});auto frozen=ha2->Snapshot().sequence;Inject(ha2->Snapshot(),ea.value_type,true,pa.address,tp,dc.interface_address,topics.ReceivePort());std::this_thread::sleep_for(50ms);assert(ha2->Snapshot().sequence==frozen);a.Command('R');Until([&]{return peer(10).session_id!=pa.session_id;});assert(ha2->Snapshot().sequence==frozen);assert(subscribe(ea,pa.session_id,ha)==-ESTALE);assert(!topics.Unsubscribe(ha2));assert(!topics.Unsubscribe(h2));pa=peer(10);assert(!subscribe(ea,pa.session_id,ha));Until([&]{return value(ha,10);});
 auto retained=ha;topics.Stop();assert(retained->Snapshot().state==n::RemoteTopicState::CLOSED);assert(hb->Snapshot().state==n::RemoteTopicState::CLOSED);assert(!topics.Start(tc));assert(!subscribe(eb,pb.session_id,hb));Until([&]{return value(hb,20);});topics.Stop();subscriber.Close();local.Stop();control.Stop();discovery.Stop();kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
 std::cout<<"Unsubscribe FD/SHM cleanup/repeat, late DATA/foreign-port INTEREST, LOST/session pin/reselection, Stop handles/restart PASS\n";
}
