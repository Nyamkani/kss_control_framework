#include "kcf/network/control_node.hpp"
#include "kcf/network/topic_protocol.hpp"
#include "kcf/network/detail/topic_proxy.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <atomic>
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <dlfcn.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <poll.h>
// Test-only SHM namespace partition for loopback virtual Hosts. Registry SHM
// stays untouched; only this test's Topic names are mapped per child process.
static char topic_namespace[80]{};
static std::string TestName(const char* name){return topic_namespace[0]&&(std::strncmp(name,"/kcf_remote_topic_",18)==0||std::strncmp(name,"/kcf_network_proxy_",19)==0)?std::string(topic_namespace)+(name+1):std::string(name);}
extern "C" int shm_open(const char* name,int flags,mode_t mode){static auto real=reinterpret_cast<int(*)(const char*,int,mode_t)>(dlsym(RTLD_NEXT,"shm_open"));return real(TestName(name).c_str(),flags,mode);}
extern "C" int shm_unlink(const char* name){static auto real=reinterpret_cast<int(*)(const char*)>(dlsym(RTLD_NEXT,"shm_unlink"));return real(TestName(name).c_str());}
struct Big {std::uint8_t data[769]{};};
namespace kcf {template<>struct TypeDescriptorTraits<Big>{static constexpr bool defined=true;static TypeDescriptor Get(){auto d=MakeTypeDescriptor<Big>("kcf.network.topic.big.v1");d.field_count=1;d.fields[0]=MakeField<Big,std::uint8_t[769]>("data",offsetof(Big,data));return d;}};}
namespace n=kcf::network;
using namespace std::chrono_literals;
n::HostIdentity Id(unsigned char x){n::HostIdentity h;h.bytes[0]=x;return h;}
auto Descriptor(){return kcf::TypeDescriptorTraits<Value>::Get();}
n::TypeIdentity Type(){n::TypeIdentity t;assert(!n::ParameterWireType(Descriptor(),t));return t;}
std::vector<std::uint8_t> Bytes(int count){Value value{count,false,1.5};kcf::DynamicPayload p;p.type_id=Type().type_id;p.bytes.resize(sizeof(value));std::memcpy(p.bytes.data(),&value,sizeof(value));std::vector<std::uint8_t> out;assert(!n::EncodeParameterValue(Descriptor(),p,out));return out;}
template<class F>void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
std::uint16_t Port(){int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));socklen_t size=sizeof(a);assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size));close(fd);return ntohs(a.sin_port);}
std::string IP(unsigned host){return "127.0.0."+std::to_string(host);}
n::ControlConfig Config(unsigned char host,std::uint16_t discovery,std::uint16_t data){n::ControlConfig c;c.discovery.host=Id(host);c.discovery.boot_id=Id(42);c.discovery.interface_address=IP(host);c.discovery.port=discovery;c.discovery.announce_interval=50ms;c.discovery.peer_timeout=350ms;c.discovery.metadata_retry=30ms;c.metadata_interval=30ms;c.reconnect_interval=50ms;c.connect_timeout=200ms;c.metadata.runtime_pids={static_cast<int>(getpid())};c.topics.enabled=true;c.topics.port=data;return c;}
struct Report {
    int code{0},count{0},local_count{0},error{0};bool enabled{false};
    std::uint64_t callbacks{0},sent{0},received{0},dropped{0},duplicate{0},out_of_order{0},missed{0},malformed{0},stale{0},skipped{0};
    unsigned send_routes{0},receive_routes{0},proxies{0};bool conflict{false},ambiguous{false},oversized{false},missing_capability{false},incompatible{false};
};
volatile std::sig_atomic_t stopped=0;
void Signal(int){stopped=1;}
int Child(unsigned host,std::uint16_t discovery,std::uint16_t data,const std::string& topic,int mode,int pipe){
    alarm(180);std::signal(SIGTERM,Signal);std::snprintf(topic_namespace,sizeof(topic_namespace),"/kcf_test_h%u_p%d_",host,getpid());
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Publisher<Value> publisher,local; kcf::Publisher<Big> big;
    if(mode){assert(!publisher.Create(topic,8));assert(!local.Create(topic+"_local"));assert(!big.Create(topic+"_big"));}
    const auto param_name="/kcf_topic_parameter_"+std::to_string(getpid());
    kcf::Parameter<Value> parameter;assert(!parameter.Create(param_name,{7,false,1.5}));
    kcf::ServiceServer service;assert(!service.Create(Port()));
    assert((!service.Register<Value,Value>(9,"/kcf_topic_service",[](const Value& input,Value& output){output=input;output.count*=2;})));assert(!service.Start());
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::ControlNode node;auto config=Config(host,discovery,data);config.metadata.remote_topics={topic,topic+"_big"};if(!mode)config.metadata.remote_topics.push_back(topic+"_local");config.metadata.remote_parameters={param_name};config.metadata.remote_services={"/kcf_topic_service"};
    config.topics.local_types={{topic,Descriptor()},{topic+"_local",Descriptor()},{topic+"_big",kcf::TypeDescriptorTraits<Big>::Get()}};
    assert(!node.Start(config));kcf::Subscriber<Value> subscriber;std::atomic<int> latest{0};std::atomic<bool> enabled{true};std::atomic<std::uint64_t> callbacks{0};bool subscribed=false;int local_count=0;
    int lock_fd=-1;kcf::detail::ChannelHeader* locked=nullptr;std::size_t lock_size=0;
    auto next=std::chrono::steady_clock::now();
    while(!stopped){
        if(mode&&std::chrono::steady_clock::now()>=next){int code=publisher.Publish({++local_count,false,1.5});assert(!code||code==-EAGAIN);next=std::chrono::steady_clock::now()+1ms;}
        pollfd p{pipe,POLLIN,0};int result=poll(&p,1,1);if(result<0){if(errno==EINTR)continue;break;}if(p.revents&(POLLHUP|POLLERR))break;
        if(!(p.revents&POLLIN))continue;
        char op=0;if(recv(pipe,&op,1,0)!=1)break;Report out;
        if(op=='U'&&!subscribed){out.code=subscriber.Create(topic,[&](const Value& v){latest=v.count;enabled=v.enabled;++callbacks;});if(!out.code)subscribed=true;}
        if(op=='O'&&subscribed){out.code=subscriber.Close();subscribed=false;}
        if(op=='N')node.Stop();
        if(op=='R')out.code=node.Start(config);
        if(op=='B'){node.Stop();config.topics.max_hz=1000;out.code=node.Start(config);}
        if(op=='A'){kcf::SharedChannel<Value> reader;out.code=reader.Open(topic);if(!out.code){Value value{};std::uint64_t sequence=0;out.code=reader.ReadLatestSnapshot(value,sequence);out.count=value.count;}}
        if(op=='K'&&!locked){lock_fd=shm_open(topic.c_str(),O_RDWR,0);assert(lock_fd>=0);struct stat st{};assert(!fstat(lock_fd,&st));lock_size=st.st_size;locked=static_cast<kcf::detail::ChannelHeader*>(mmap(nullptr,lock_size,PROT_READ|PROT_WRITE,MAP_SHARED,lock_fd,0));assert(locked!=MAP_FAILED);assert(!pthread_mutex_lock(&locked->notify_mutex));}
        if(op=='J'&&locked){assert(!pthread_mutex_unlock(&locked->notify_mutex));munmap(locked,lock_size);close(lock_fd);locked=nullptr;lock_fd=-1;}
        auto stats=node.GetTopicDiagnostics();out.send_routes=stats.active_send_routes;out.receive_routes=stats.active_receive_routes;out.proxies=stats.proxies.size();out.malformed=stats.malformed;out.stale=stats.stale;
        for(const auto& proxy:stats.proxies)assert(proxy.is_proxy&&proxy.source.runtime.host!=Id(host));
        for(const auto& r:stats.routes){out.sent+=r.packets_sent;out.received+=r.packets_received;out.dropped+=r.dropped;out.duplicate+=r.duplicate;out.out_of_order+=r.out_of_order;out.missed+=r.missed_sequence;out.skipped+=r.local_samples_skipped;
            out.conflict|=r.error==-EEXIST;out.ambiguous|=r.error==-EADDRINUSE;out.oversized|=r.error==-EMSGSIZE;out.missing_capability|=r.reason==n::CompatibilityReason::CAPABILITY_MISSING;out.incompatible|=r.reason==n::CompatibilityReason::NETWORK_MAJOR_MISMATCH;
        }
        out.error=node.GetLastError();out.callbacks=callbacks;out.enabled=enabled;out.local_count=local_count;if(op!='A')out.count=latest;
        assert(send(pipe,&out,sizeof(out),MSG_NOSIGNAL)==sizeof(out));if(op=='Q')break;
    }
    if(locked){pthread_mutex_unlock(&locked->notify_mutex);munmap(locked,lock_size);close(lock_fd);}
    subscriber.Close();node.Stop();service.Stop();parameter.Close();parameter.Unlink();if(mode){publisher.Close();publisher.Unlink();local.Close();local.Unlink();big.Close();big.Unlink();}
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);return 0;
}
struct HostChild {
    pid_t pid;int fd;unsigned host;std::string topic;
    HostChild(unsigned h,std::uint16_t discovery,std::uint16_t data,std::string name,int mode):host(h),topic(std::move(name)){
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));auto hs=std::to_string(h),ds=std::to_string(discovery),ts=std::to_string(data),ms=std::to_string(mode),fs=std::to_string(pair[1]);pid=fork();assert(pid>=0);
        if(!pid){close(pair[0]);fcntl(pair[1],F_SETFD,0);execl("/proc/self/exe","kcf_topic_data_test","--host",hs.c_str(),ds.c_str(),ts.c_str(),topic.c_str(),ms.c_str(),fs.c_str(),nullptr);_exit(127);}close(pair[1]);fd=pair[0];
    }
    Report Ask(char op='S'){assert(send(fd,&op,1,MSG_NOSIGNAL)==1);pollfd p{fd,POLLIN,0};assert(poll(&p,1,10000)>0);Report r;assert(recv(fd,&r,sizeof(r),0)==sizeof(r));return r;}
    void End(){if(pid<=0)return;Ask('Q');int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));close(fd);
        std::string prefix="/kcf_test_h"+std::to_string(host)+"_p"+std::to_string(pid)+"_";for(const auto& suffix:{"","_local","_big"})shm_unlink((prefix+topic.substr(1)+suffix).c_str());pid=0;}
    ~HostChild(){if(pid>0){kill(pid,SIGTERM);waitpid(pid,nullptr,0);close(fd);}}
};
n::PeerInfo Peer(n::ControlNode& node,unsigned host){for(const auto& p:node.GetPeers())if(p.host==Id(host))return p;return {};}
void ControlTraffic(n::ControlNode& client,const HostChild& owner){
    Until([&]{return Peer(client,owner.host).metadata_ready;});n::RemoteTarget target;target.scope=n::TargetScope::HOST;target.host=Id(owner.host);
    std::vector<std::future<n::ParameterBatchResult>> futures;
    for(unsigned i=0;i<12;++i){futures.push_back(client.Get(target,"/kcf_topic_parameter_"+std::to_string(owner.pid),Type(),2s));futures.push_back(client.CallService(target,"/kcf_topic_service",Type(),Type(),Bytes(3),2s));}
    for(auto& f:futures){assert(f.wait_for(4s)==std::future_status::ready);auto r=f.get();assert(!r.error&&r.endpoints.size()==1&&!r.endpoints[0].error);}
}
void Integration(){const auto discovery=Port(),data=Port();auto topic="/kcf_remote_topic_live_"+std::to_string(getpid());
    HostChild a(1,discovery,data,topic,1),b(2,discovery,data,topic,0),c(3,discovery,data,topic,0);
    Until([&]{return b.Ask().proxies==1&&c.Ask().proxies==1;});assert(a.Ask().send_routes==0&&b.Ask().received==0&&c.Ask().received==0);assert(b.Ask().oversized);
    Until([&]{return b.Ask('U').code==0;});Until([&]{return b.Ask().callbacks>5&&a.Ask().send_routes==1;});auto initial=b.Ask();assert(!initial.enabled&&initial.count>0&&c.Ask().received==0);
    auto before=a.Ask();std::this_thread::sleep_for(1100ms);auto after=a.Ask();assert(after.sent-before.sent>=15&&after.sent-before.sent<=58&&after.local_count-before.local_count>200&&after.skipped>0);
    Until([&]{return c.Ask('U').code==0;});Until([&]{return c.Ask().callbacks>3&&a.Ask().send_routes==2;});
    assert(!b.Ask('O').code);Until([&]{return a.Ask().send_routes==1;});assert(!b.Ask('A').code&&b.Ask().proxies==1);auto no_sub=b.Ask();std::this_thread::sleep_for(150ms);assert(b.Ask().callbacks==no_sub.callbacks);
    n::ControlNode control;auto config=Config(4,discovery,data);config.topics.enabled=false;config.metadata.runtime_pids={-1};assert(!control.Start(config));
    Until([&]{auto peer=Peer(control,2);if(!peer.metadata_ready)return false;for(const auto& e:peer.endpoints)assert(e.kind!=n::EndpointKind::TOPIC||e.role!=n::EndpointRole::PROVIDER);return true;});
    assert(!a.Ask('B').code);Until([&]{return a.Ask().send_routes==1;});ControlTraffic(control,a);assert(c.Ask().callbacks>3);assert(a.Ask().send_routes==1&&!a.Ask().error);
    auto prior=c.Ask();a.Ask('N');Until([&]{return c.Ask().receive_routes==0;});assert(!c.Ask('A').code);assert(!a.Ask('R').code);Until([&]{return c.Ask().callbacks>prior.callbacks&&c.Ask().receive_routes==1;});
    control.Stop();a.End();b.End();c.End();std::cout<<"metadata/allowlist, 2/3 peers, no-subscriber no-send, rate-limit, proxy no-loop, Control-under-load, LOST/restart PASS\n";
}
void Conflict(){auto discovery=Port(),data=Port();auto topic="/kcf_remote_topic_conflict_"+std::to_string(getpid());HostChild a(5,discovery,data,topic,1),b(6,discovery,data,topic,1),c(7,discovery,data,topic,0);
    Until([&]{return a.Ask().conflict&&b.Ask().conflict&&c.Ask().ambiguous;});assert(!b.Ask('U').code);std::this_thread::sleep_for(300ms);assert(!a.Ask().send_routes&&!b.Ask().send_routes&&!c.Ask().receive_routes);a.End();b.End();c.End();std::cout<<"existing Local publisher and ambiguous multi-source conflict PASS\n";
}
n::detail::ProxyOwnership Ownership(){n::detail::ProxyOwnership o;o.proxy={{Id(2),Id(42),getpid(),0},(1ull<<61)|1};assert(kcf::detail::ReadProcessIdentity(getpid(),o.proxy.runtime.process_start_ticks));o.source={{Id(1),Id(42),123,1},1};o.gateway_session=o.source_session=1;return o;}
void ProxyStorage(){auto name="/kcf_remote_topic_storage_"+std::to_string(getpid());
    n::detail::TopicProxy proxy;assert(!proxy.Create(name,Descriptor(),Ownership()));kcf::SharedChannel<Value> reader;assert(!reader.Open(name)&&reader.GetDepth()==1);
    kcf::DynamicPayload value;assert(!n::DecodeParameterValue(Descriptor(),Bytes(10),value));assert(!proxy.Publish(value));Value received{};kcf::TopicReadInfo info;assert(!reader.ReadLatest(received,info)&&received.count==10&&!received.enabled&&info.sequence==1);
    int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);struct stat st{};assert(!fstat(fd,&st));auto* base=static_cast<unsigned char*>(mmap(nullptr,st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(base!=MAP_FAILED);auto* h=reinterpret_cast<kcf::detail::ChannelHeader*>(base);assert(h->format==4);
    kcf::detail::ChannelLayout layout;assert(kcf::detail::ComputeChannelLayout(sizeof(Value),alignof(Value),1,layout));
    for(unsigned i=0;i<3;++i)reinterpret_cast<kcf::detail::ChannelSlot<std::byte>*>(base+layout.slots+i*layout.stride)->users.store(1);
    assert(proxy.Publish(value)==-EAGAIN&&h->publish_sequence.load()==1);
    for(unsigned i=0;i<3;++i)reinterpret_cast<kcf::detail::ChannelSlot<std::byte>*>(base+layout.slots+i*layout.stride)->users.store(0);
    auto child=fork();assert(child>=0);if(!child){assert(!pthread_mutex_lock(&h->notify_mutex));_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status));assert(!proxy.Publish(value)&&h->publish_sequence.load()==2);
    assert(!shm_unlink(name.c_str()));kcf::Publisher<Value> replacement;assert(!replacement.Create(name));assert(proxy.Publish(value)==-ESTALE);assert(!replacement.Publish({20,false,1}));replacement.Close();replacement.Unlink();reader.Close();munmap(base,st.st_size);close(fd);
    child=fork();assert(child>=0);if(!child){kcf::Publisher<Value> dead;assert(!dead.Create(name));_exit(0);}assert(waitpid(child,&status,0)==child);n::detail::TopicProxy conflict;assert(conflict.Create(name,Descriptor(),Ownership())==-EEXIST);assert(!shm_unlink(name.c_str()));
    shm_unlink(n::detail::ProxySidecarName(name).c_str());
    std::cout<<"proxy Format 4, dead-reader pin/no overwrite, robust owner death, stale object, no implicit reclaim PASS\n";
}
void Codec(){n::TopicPacket packet;packet.source={{Id(1),Id(42),123,1},1};packet.destination={{Id(2),Id(42),234,1},2};packet.source_session=3;packet.destination_session=4;packet.route_id=5;packet.name="/test";packet.type=Type();std::vector<std::uint8_t> bytes;
    assert(!n::EncodeTopicPacket(packet,bytes)&&bytes.size()==n::TOPIC_DATA_HEADER+5);n::TopicPacket copy;assert(!n::DecodeTopicPacket(bytes.data(),bytes.size(),copy));
    packet.kind=n::TopicMessage::DATA;packet.sequence=1;packet.sample_sequence=2;packet.payload=Bytes(1);assert(!n::EncodeTopicPacket(packet,bytes));
    for(std::size_t size=0;size<bytes.size();++size)assert(n::DecodeTopicPacket(bytes.data(),size,copy));
    for(auto offset:{0,5,7,9,11,13,15}){auto bad=bytes;bad[offset]=255;assert(n::DecodeTopicPacket(bad.data(),bad.size(),copy));}
    packet.type.payload_size=769;assert(n::EncodeTopicPacket(packet,bytes)==-EMSGSIZE);std::cout<<"Topic BE wire/malformed/version/size/oversize PASS\n";
}
int DataSocket(unsigned host,std::uint16_t port){int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);assert(inet_pton(AF_INET,IP(host).c_str(),&a.sin_addr)==1);assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));return fd;}
n::TopicPacket ReadInterest(int fd){for(unsigned attempt=0;attempt<30;++attempt){pollfd p{fd,POLLIN,0};if(poll(&p,1,200)<=0)continue;std::uint8_t data[1201];auto count=recv(fd,data,sizeof(data),0);n::TopicPacket packet;if(count>0&&!n::DecodeTopicPacket(data,count,packet)&&packet.kind==n::TopicMessage::INTEREST)return packet;}assert(false);return {};}
void SendBytes(int fd,unsigned host,std::uint16_t port,const std::vector<std::uint8_t>& bytes){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,IP(host).c_str(),&a.sin_addr);assert(sendto(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&a),sizeof(a))==static_cast<ssize_t>(bytes.size()));}
void SendData(int fd,unsigned host,std::uint16_t port,n::TopicPacket packet,std::uint64_t sequence,int value){packet.kind=n::TopicMessage::DATA;packet.sequence=sequence;packet.sample_sequence=sequence;packet.payload=Bytes(value);std::vector<std::uint8_t> bytes;assert(!n::EncodeTopicPacket(packet,bytes));SendBytes(fd,host,port,bytes);}
void DatagramBehavior(){auto discovery_port=Port(),data_port=Port();auto name="/kcf_remote_topic_udp_"+std::to_string(getpid());HostChild receiver(11,discovery_port,data_port,name,0);
    n::PeerDiscovery source;auto config=Config(10,discovery_port,data_port).discovery;
    config.profile.capabilities=n::CapabilityBit(n::Capability::DISCOVERY)|n::CapabilityBit(n::Capability::TARGET_ENDPOINT);
    n::NetworkEndpointMetadata endpoint;endpoint.identity.runtime={Id(10),Id(42),getpid(),0};assert(kcf::detail::ReadProcessIdentity(getpid(),endpoint.identity.runtime.process_start_ticks));endpoint.identity.registration_id=1;endpoint.name=name;endpoint.element_name="synthetic_topic_source";endpoint.scope=n::NetworkScope::REMOTE;endpoint.value_type=Type();
    assert(!source.Start(config,{endpoint}));Until([&]{return receiver.Ask().missing_capability;});assert(!receiver.Ask().proxies);
    source.Stop();config.profile.capabilities|=n::CapabilityBit(n::Capability::REMOTE_TOPIC);config.profile.network.major=2;assert(!source.Start(config,{endpoint}));Until([&]{return receiver.Ask().incompatible;});assert(!receiver.Ask().proxies);
    source.Stop();config.profile.network.major=1;auto wrong=endpoint;wrong.value_type.type_id++;assert(!source.Start(config,{wrong}));std::this_thread::sleep_for(400ms);assert(!receiver.Ask().receive_routes&&!receiver.Ask().proxies);
    assert(!source.UpdateMetadata({endpoint}));Until([&]{return receiver.Ask().proxies==1;});int fd=DataSocket(10,data_port);Until([&]{return receiver.Ask('U').code==0;});auto interest=ReadInterest(fd);
    SendData(fd,11,data_port,interest,1,1);Until([&]{return receiver.Ask().count==1;});SendData(fd,11,data_port,interest,1,99);Until([&]{return receiver.Ask().duplicate>=1;});assert(receiver.Ask().count==1);
    SendData(fd,11,data_port,interest,3,3);Until([&]{return receiver.Ask().count==3;});SendData(fd,11,data_port,interest,2,2);Until([&]{return receiver.Ask().out_of_order>=1;});assert(receiver.Ask().missed==1&&receiver.Ask().count==3);
    SendData(fd,11,data_port,interest,4,4);Until([&]{return receiver.Ask().count==4;});
    auto bad=interest;bad.kind=n::TopicMessage::DATA;bad.sequence=bad.sample_sequence=5;bad.payload=Bytes(5);bad.payload[4]=2;std::vector<std::uint8_t> bytes;assert(!n::EncodeTopicPacket(bad,bytes));SendBytes(fd,11,data_port,bytes);
    bytes[0]^=0xff;SendBytes(fd,11,data_port,bytes);SendBytes(fd,11,data_port,std::vector<std::uint8_t>(1201,0));Until([&]{return receiver.Ask().malformed>=3;});assert(receiver.Ask().count==4);
    // Burst input is coalesced, and a held Local notify mutex cannot block the Data worker.
    assert(!receiver.Ask('K').code);for(unsigned i=6;i<=1500;++i)SendData(fd,11,data_port,interest,i,i);Until([&]{return receiver.Ask().dropped>0;});assert(receiver.Ask().count==4);
    n::ControlNode control;auto cc=Config(12,discovery_port,data_port);cc.topics.enabled=false;cc.metadata.runtime_pids={-1};assert(!control.Start(cc));ControlTraffic(control,receiver);
    assert(!receiver.Ask('J').code);SendData(fd,11,data_port,interest,2000,2000);Until([&]{return receiver.Ask().count==2000;});assert(receiver.Ask().missed>1&&receiver.Ask().receive_routes==1&&!receiver.Ask().error);
    auto stale=interest;source.Stop();Until([&]{return receiver.Ask().receive_routes==0;});assert(!receiver.Ask('A').code);
    assert(!source.Start(config,{endpoint}));n::TopicPacket current;Until([&]{current=ReadInterest(fd);return current.source_session==source.GetSessionId();});assert(current.route_id!=stale.route_id);
    auto old_stats=receiver.Ask();SendData(fd,11,data_port,stale,3000,3000);Until([&]{return receiver.Ask().stale>old_stats.stale;});assert(receiver.Ask().count==2000);SendData(fd,11,data_port,current,1,4000);Until([&]{return receiver.Ask().count==4000;});
    auto local=endpoint;local.scope=n::NetworkScope::LOCAL;assert(!source.UpdateMetadata({local}));Until([&]{return receiver.Ask().receive_routes==0;});assert(!receiver.Ask('A').code);
    control.Stop();source.Stop();close(fd);receiver.End();std::cout<<"capability/version/type gates, duplicate/reorder/loss, malformed/oversize, bounded latest under blocked proxy, Control isolation, stale session PASS\n";
}
void Startup(){auto d=Port(),p=Port();int occupied=DataSocket(15,p);n::ControlNode node;auto c=Config(15,d,p);assert(node.Start(c)==-EADDRINUSE);assert(node.GetPeers().empty());close(occupied);c.topics.max_hz=0;assert(node.Start(c)==-EINVAL);c.topics.max_hz=50;assert(!node.Start(c));node.Stop();assert(!node.Start(c));node.Stop();std::cout<<"Data Plane bind failure/validation/Start-Stop cleanup PASS\n";}
int main(int argc,char** argv){if(argc==8&&std::string(argv[1])=="--host")return Child(std::stoi(argv[2]),std::stoi(argv[3]),std::stoi(argv[4]),argv[5],std::stoi(argv[6]),std::stoi(argv[7]));std::cout.setf(std::ios::unitbuf);alarm(180);Codec();ProxyStorage();Startup();Integration();Conflict();DatagramBehavior();std::cout<<"Remote Topic PASS\n";}
