#ifdef NDEBUG
#undef NDEBUG
#endif
#include "r5_test_types.hpp"
#include "kcf/dynamic/dynamic_service_client.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/service_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include "bringup/bringup.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
using namespace kcf;
using namespace std::chrono_literals;
namespace {
bool stop_copy=false;int copy_notice=-1;
void Send(int fd,int value){assert(write(fd,&value,sizeof(value))==sizeof(value));}
int Receive(int fd){pollfd p{fd,POLLIN,0};assert(poll(&p,1,10000)==1);int n;assert(read(fd,&n,sizeof(n))==sizeof(n));return n;}
void Reap(pid_t pid,int signal=0){int s;assert(waitpid(pid,&s,0)==pid);if(signal)assert(WIFSIGNALED(s)&&WTERMSIG(s)==signal);else assert(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(5ms);}}
struct Socket {
    int fd;std::uint16_t port;
    Socket(){fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);port=ntohs(a.sin_port);}
    ~Socket(){close(fd);}
};
std::uint16_t CreateFree(ServiceServer& server){for(int i=0;i<20;++i){std::uint16_t port;{Socket s;port=s.port;}const int r=server.Create(port);if(!r)return port;assert(r==-EADDRINUSE);}assert(false);return 0;}
template<class T> DynamicPayload Bytes(const T& value){DynamicPayload p;p.type_id=TypeDescriptorTraits<T>::Get().type_id;p.bytes.resize(sizeof(T));std::memcpy(p.bytes.data(),&value,sizeof(value));return p;}
template<class T> T Decode(const DynamicPayload& p){assert(p.bytes.size()==sizeof(T));T v;std::memcpy(&v,p.bytes.data(),sizeof(v));return v;}
auto RequestType(){return TypeDescriptorTraits<R5Request>::Get();}
auto ResponseType(){return TypeDescriptorTraits<R5Response>::Get();}
void Request(int socket,std::uint16_t port,const detail::ServicePacket& p){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);const auto n=sizeof(ServiceHeader)+p.header.payload_size;assert(sendto(socket,&p,n,0,reinterpret_cast<sockaddr*>(&a),sizeof(a))==static_cast<ssize_t>(n));}
detail::ServicePacket Reply(int socket,sockaddr_in* sender=nullptr){pollfd p{socket,POLLIN,0};assert(poll(&p,1,3000)==1);detail::ServicePacket reply;sockaddr_in a{};socklen_t n=sizeof(a);auto count=recvfrom(socket,&reply,sizeof(reply),MSG_TRUNC,reinterpret_cast<sockaddr*>(&a),&n);assert(count>=static_cast<ssize_t>(sizeof(ServiceHeader)));if(sender)*sender=a;return reply;}
void SendReply(int socket,const sockaddr_in& a,const detail::ServicePacket& p){auto n=sizeof(ServiceHeader)+p.header.payload_size;assert(sendto(socket,&p,n,0,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==static_cast<ssize_t>(n));}
void Transport(){
    ServiceServer server;const auto port=CreateFree(server);std::atomic<int> calls{0};
    assert((server.Register<R5Request,R5Response>(10,"/r5/test",[&](const auto& q,auto& r){r={q.x+q.y,++calls};})==0));
    assert((server.Register<R5Legacy,R5Legacy>(11,[](const auto& q,auto& r){r={q.x+q.y,0};})==0));assert(server.Start()==0);
    DynamicServiceClient dynamic;assert(dynamic.Open(port,10,RequestType(),ResponseType())==0);DynamicPayload response;
    assert(dynamic.Call(Bytes(R5Request{2,3}),response)==0);auto value=Decode<R5Response>(response);assert(value.x==5&&value.y==1&&response.type_id==ResponseType().type_id);
    ServiceClient typed;assert(typed.Create(port)==0);R5Response out{};assert(typed.Call(10,R5Request{3,4},out)==0&&out.x==7&&out.y==2);
    const auto saved=response;auto bad=Bytes(R5Request{});bad.type_id++;assert(dynamic.Call(bad,response)==-EPROTOTYPE);bad=Bytes(R5Request{});bad.bytes.pop_back();assert(dynamic.Call(bad,response)==-EMSGSIZE);
    DynamicServiceClient wrong;
    assert(wrong.Open(port,10,TypeDescriptorTraits<R5WrongRequest>::Get(),ResponseType())==0);
    assert(wrong.Call(Bytes(R5WrongRequest{}),response)==-EPROTOTYPE);wrong.Close();
    assert(wrong.Open(port,10,RequestType(),TypeDescriptorTraits<R5WrongResponse>::Get())==0);
    assert(wrong.Call(Bytes(R5Request{}),response)==-EPROTOTYPE);wrong.Close();
    assert(wrong.Open(port,10,TypeDescriptorTraits<R5Conflict>::Get(),ResponseType())==0);
    assert(wrong.Call(Bytes(R5Conflict{}),response)==-EPROTOTYPE);wrong.Close();
    assert(calls.load()==2&&response.type_id==saved.type_id&&response.bytes==saved.bytes);
    R5Legacy legacy{};assert(typed.Call(11,R5Legacy{4,5},legacy)==0&&legacy.x==9);
    auto zero=RequestType();zero.type_id=0;assert(wrong.Open(port,11,zero,ResponseType())==-EINVAL);
    assert(wrong.Open(port,11,RequestType(),ResponseType())==0);assert(wrong.Call(Bytes(R5Request{}),response)==-EPROTOTYPE);wrong.Close();
    Socket raw;detail::ServicePacket q;q.header.service_id=10;q.header.client_id=123;q.header.request_id=456;q.header.payload_size=sizeof(R5Request);
    const auto req=detail::DeclaredStorageIdentity<R5Request>(),res=detail::DeclaredStorageIdentity<R5Response>();
    q.header.request_type_id=req.type_id;q.header.response_type_id=res.type_id;q.header.request_layout_id=req.layout_id;q.header.response_layout_id=res.layout_id;
    R5Request input{8,9};std::memcpy(q.payload.data(),&input,sizeof(input));Request(raw.fd,port,q);auto first=Reply(raw.fd);assert(first.header.framework_status==0&&calls.load()==3);
    Request(raw.fd,port,q);auto duplicate=Reply(raw.fd);assert(calls.load()==3&&std::memcmp(first.payload.data(),duplicate.payload.data(),sizeof(R5Response))==0);
    q.payload[0]^=1;Request(raw.fd,port,q);assert(Reply(raw.fd).header.framework_status==-EINVAL);q.payload[0]^=1;
    q.header.request_type_id++;Request(raw.fd,port,q);assert(Reply(raw.fd).header.framework_status==-EINVAL);q.header.request_type_id--;
    q.header.response_layout_id++;Request(raw.fd,port,q);assert(Reply(raw.fd).header.framework_status==-EINVAL);assert(calls.load()==3);
    q.header.request_id++;q.header.response_layout_id--;q.header.protocol_version=1;Request(raw.fd,port,q);pollfd p{raw.fd,POLLIN,0};assert(poll(&p,1,80)==0&&calls.load()==3);
    typed.Close();dynamic.Close();server.Stop();
    std::uint64_t ticks;assert(detail::ReadProcessIdentity(getpid(),ticks));RuntimeInfo self{};self.pid=getpid();self.process_start_ticks=ticks;std::vector<ServiceInfo> services;IntrospectionClient c;assert(c.ListServices(self,services)==-ENOENT);
    std::cout<<"PASS dynamic/typed interoperability, equal-size wrong request/response/layout rejection before callback, legacy 0/0, duplicate payload/type protection, old protocol rejected, no-Runtime Service\n";
}
void FakeResponses(){
    for(int mode=0;mode<10;++mode){Socket server;
        std::thread worker([&]{sockaddr_in sender{};auto q=Reply(server.fd,&sender);
            if(mode==0){auto retry=Reply(server.fd,&sender);assert(std::memcmp(&q.header,&retry.header,sizeof(ServiceHeader))==0&&q.payload==retry.payload);q=retry;}
            auto r=q;r.header.packet_type=ServicePacketType::RESPONSE;r.header.payload_size=sizeof(R5Response);R5Response v{33,1};std::memcpy(r.payload.data(),&v,sizeof(v));
            if(mode==1)r.header.client_id++;
            if(mode==2)r.header.request_id++;
            if(mode==3)r.header.service_id++;
            if(mode==4)r.header.request_type_id++;
            if(mode==5)r.header.response_type_id++;
            if(mode==6)r.header.response_layout_id++;
            if(mode==7)r.header.payload_size--;
            if(mode==8)r.header.protocol_version=1;
            if(mode==9){Socket foreign;SendReply(foreign.fd,sender,r);}else SendReply(server.fd,sender,r);
        });
        DynamicServiceClient c;assert(c.Open(server.port,10,RequestType(),ResponseType(),30,mode==0?1:0)==0);
        DynamicPayload out=Bytes(R5Response{99,0});const auto saved=out;
        const auto result=c.Call(Bytes(R5Request{1,2}),out);
        if(mode==0)assert(result==0&&Decode<R5Response>(out).x==33);
        else {const int expected=mode>=4&&mode<=6?-EPROTOTYPE:mode==7?-EMSGSIZE:-ETIMEDOUT;assert(result==expected);assert(out.bytes==saved.bytes&&out.type_id==saved.type_id);}
        worker.join();c.Close();
    }
    Socket unused;DynamicServiceClient c;assert(c.Open(unused.port,10,RequestType(),ResponseType(),20,2)==0);
    DynamicPayload out;auto start=std::chrono::steady_clock::now();assert(c.Call(Bytes(R5Request{}),out)==-ETIMEDOUT);assert(std::chrono::steady_clock::now()-start>=50ms);
    std::cout<<"PASS retry preserves IDs/bytes, timeout budget, response client/request/service/type/layout/size/protocol/source checks, failed output preservation\n";
}
struct Element:ProcessElement {
    ServiceServer server;int command=-1,reply=-1;int mode=0;std::uint16_t port=0;int blocker=-1;std::string blocked_name;
    explicit Element(int m=0):mode(m){}
    void Configure(){port=CreateFree(server);
        assert((server.Register<R5Request,R5Response>(10,"/r5/test",[](const auto& q,auto& r){r={q.x+q.y,1};})==0));
        assert((server.Register<R5Legacy,R5Legacy>(11,[](const auto& q,auto& r){r=q;})==0));
        assert((server.Register<R5Conflict,R5Response>(12,"/r5/conflict",[](const auto& q,auto& r){r={q.x,q.y};})==0));
        if(mode==1)for(unsigned i=100;i<165;++i)assert((server.Register<R5Request,R5Response>(i,[](const auto& q,auto& r){r={q.x,q.y};})==0));
        assert(server.Start()==0);
    }
    int Setup() override {
        if(mode==3){std::uint64_t ticks;assert(detail::ReadProcessIdentity(getpid(),ticks));blocked_name=detail::ServiceRegistryName(getpid(),ticks);
            blocker=shm_open(blocked_name.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);assert(blocker>=0&&flock(blocker,LOCK_EX)==0);}
        if(mode==2)for(unsigned i=0;i<detail::MAX_TYPES;++i){auto d=RequestType();auto name="kcf.r5.full."+std::to_string(i);std::memset(d.type_name,0,sizeof(d.type_name));std::memcpy(d.type_name,name.data(),name.size());d.type_id=StableTypeId(name);assert(detail::RegisterType(d));}
        Configure();return 0;
    }
    int Loop() override {if(command>=0){pollfd p{command,POLLIN,0};if(poll(&p,1,0)==1){auto action=Receive(command);if(action==1)server.Stop();if(action==2)Configure();Send(reply,action==3?port:action);}}return 0;}
    void Shutdown() override {server.Stop();if(blocker>=0){close(blocker);assert(shm_unlink(blocked_name.c_str())==0);}}
};
RuntimeInfo Runtime(pid_t pid){RuntimeInfo r{};Until([&]{IntrospectionClient c;std::vector<RuntimeInfo> list;assert(c.ListRuntimes(list)==0);for(const auto& value:list)if(value.pid==pid&&value.state==ProcessState::RUNNING){r=value;return true;}return false;});return r;}
void Check(const RuntimeInfo& r,bool full=false){IntrospectionClient c;std::vector<ServiceInfo> entries;assert(c.ListServices(r,entries)==0);bool named=false,legacy=false,conflict=false;
    for(const auto& s:entries){assert(s.port&&s.registration_id&&s.request_size==8&&s.response_size==8);
        if(s.service_id==10){named=true;assert(std::string(s.name)=="/r5/test");if(full)assert(!s.request_type_id&&!s.response_type_id);else{
            TypeDescriptor request,response;assert(c.GetType(r,s.request_type_id,request)==0&&c.GetType(r,s.response_type_id,response)==0);
            assert(request.type_id==RequestType().type_id&&response.type_id==ResponseType().type_id);DynamicServiceClient client;
            assert(client.Open(s.port,s.service_id,request,response)==0);DynamicPayload value;assert(client.Call(Bytes(R5Request{6,7}),value)==0&&Decode<R5Response>(value).x==13);
        }}
        if(s.service_id==11){legacy=true;assert(!s.request_type_id&&!s.response_type_id&&std::string(s.name)=="service@"+std::to_string(s.port)+":11");}
        if(s.service_id==12){conflict=true;assert(!s.request_type_id);}
    }assert(named&&legacy&&conflict);
}
void Lifecycle(int mode){int commands[2],replies[2];assert(pipe(commands)==0&&pipe(replies)==0);auto child=fork();assert(child>=0);
    if(!child){close(commands[1]);close(replies[0]);Element e(mode);e.command=commands[0];e.reply=replies[1];ProcessRuntime r;r.SetLoopFrequency(500);_exit(r.Run(e)?1:0);}
    close(commands[0]);close(replies[1]);auto runtime=Runtime(child);
    if(mode==3){IntrospectionClient client;std::vector<ServiceInfo> services;assert(client.ListServices(runtime,services)<0);
        Send(commands[1],3);const auto port=Receive(replies[0]);ServiceClient call;assert(call.Create(port)==0);R5Response out{};
        assert(call.Call(10,R5Request{7,8},out)==0&&out.x==15);call.Close();assert(kill(child,SIGTERM)==0);Reap(child);
        close(commands[1]);close(replies[0]);std::cout<<"PASS Service registry creation failure does not affect Start/callback/Stop\n";return;}
    Check(runtime,mode==2);IntrospectionClient c;std::vector<ServiceInfo> entries;assert(c.ListServices(runtime,entries)==0);auto info=entries.front();
    if(mode==1){assert(entries.size()==detail::MAX_SERVICES);ServiceClient client;assert(client.Create(info.port)==0);R5Response out{};assert(client.Call(164,R5Request{1,2},out)==0&&out.x==1);}
    if(mode==2){ServiceClient client;assert(client.Create(info.port)==0);R5Response out{};assert(client.Call(10,R5Request{2,3},out)==0&&out.x==5);}
    if(mode==0){int ready[2];assert(pipe(ready)==0);auto observer=fork();assert(observer>=0);
        if(!observer){close(ready[0]);copy_notice=ready[1];stop_copy=true;std::vector<ServiceInfo> copy;(void)c.ListServices(runtime,copy);_exit(2);}
        close(ready[1]);assert(Receive(ready[0])==1);close(ready[0]);int status;assert(waitpid(observer,&status,WUNTRACED)==observer&&WIFSTOPPED(status));
        Check(runtime);assert(kill(observer,SIGKILL)==0);Reap(observer,SIGKILL);Check(runtime);
        Send(commands[1],1);assert(Receive(replies[0])==1);assert(c.ListServices(runtime,entries)==0&&entries.empty());
        Send(commands[1],2);assert(Receive(replies[0])==2);Check(runtime);assert(c.ListServices(runtime,entries)==0&&entries.front().registration_id>info.registration_id);
        using Payload=detail::ServiceRegistrySnapshot;
        using Slot=detail::ChannelSlot<Payload>;
        detail::ChannelLayout layout;assert(detail::ComputeChannelLayout(sizeof(Payload),alignof(Payload),1,layout));
        auto name=detail::ServiceRegistryName(runtime.pid,runtime.process_start_ticks);int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);
        auto* mapped=static_cast<unsigned char*>(mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(mapped!=MAP_FAILED);
        for(unsigned i=0;i<3;++i)reinterpret_cast<Slot*>(mapped+layout.slots+i*layout.stride)->users.store(1);
        Send(commands[1],1);assert(Receive(replies[0])==1);assert(c.ListServices(runtime,entries)==-ENOENT);
        assert(munmap(mapped,layout.length)==0);close(fd);
        Send(commands[1],2);assert(Receive(replies[0])==2);Check(runtime);
    }
    assert(kill(child,mode==2?SIGKILL:SIGTERM)==0);Reap(child,mode==2?SIGKILL:0);assert(c.ListServices(runtime,entries)==-ENOENT);
    const auto name=detail::ServiceRegistryName(runtime.pid,runtime.process_start_ticks);int fd=shm_open(name.c_str(),O_RDONLY,0);
    if(mode==2){assert(fd>=0);close(fd);assert(shm_unlink(name.c_str())==0);shm_unlink(detail::RuntimeRegistryName(runtime.pid,runtime.process_start_ticks).c_str());shm_unlink(detail::EndpointRegistryName(runtime.pid,runtime.process_start_ticks).c_str());shm_unlink(detail::TypeRegistryName(runtime.pid,runtime.process_start_ticks).c_str());}
    else assert(fd<0&&errno==ENOENT);
    close(commands[1]);close(replies[0]);
    std::cout<<"PASS Runtime discovery/descriptor chain/lifecycle mode="<<mode<<" (observer, capacity or type-full/stale)\n";
}
void Malformed(){
    std::uint64_t ticks;assert(detail::ReadProcessIdentity(getpid(),ticks));RuntimeInfo runtime{};runtime.pid=getpid();runtime.process_start_ticks=ticks;
    detail::ServiceRegistrySnapshot snapshot{};snapshot.pid=getpid();snapshot.process_start_ticks=ticks;
    SharedChannel<detail::ServiceRegistrySnapshot> owner;assert(owner.Create(detail::ServiceRegistryName(runtime.pid,ticks))==0);
    IntrospectionClient c;std::vector<ServiceInfo> output(1);output[0].registration_id=999;
    for(int mode=0;mode<12;++mode){auto value=snapshot;value.service_count=1;auto& e=value.services[0];e.registration_id=1;e.port=1234;e.request_size=8;e.response_size=8;std::strcpy(e.name,"/r5/malformed");
        switch(mode){case 0:value.protocol_version++;break;case 1:value.struct_size++;break;case 2:value.process_start_ticks++;break;
        case 3:value.service_count=65;break;case 4:e.port=0;break;case 5:e.request_size=SERVICE_MAX_PAYLOAD+1;break;
        case 6:e.registration_id=0;break;case 7:std::memset(e.name,'x',sizeof(e.name));break;
        case 8:value.service_count=2;value.services[1]=e;break;case 9:e.request_type_id=RequestType().type_id;break;
        case 10:e.response_size=0;break;case 11:std::memset(e.diagnostic_request_type_name,'x',sizeof(e.diagnostic_request_type_name));break;}
        assert(owner.TryPublishSnapshot(value)==0);assert(c.ListServices(runtime,output)<0&&output.size()==1&&output[0].registration_id==999);
    }
    assert(owner.TryPublishSnapshot(snapshot)==0&&c.ListServices(runtime,output)==0&&output.empty());
    assert(owner.Close()==0&&owner.Unlink()==0);
    std::cout<<"PASS malformed Service snapshot/generation/count/port/size/ID/string/type consistency rejected without changing output\n";
}
void Supervised(const char* executable){auto pid=fork();assert(pid>=0);if(!pid){bringup::Bringup app;if(app.Setup("r5_application",{{"service",executable,{"--child"}}}))_exit(2);_exit(app.Run());}
    IntrospectionClient c;RuntimeInfo runtime{};Until([&]{std::vector<SupervisorInfo> list;assert(c.ListSupervisors(list)==0);for(const auto& s:list)if(s.pid==pid&&s.application_state==ApplicationState::RUNNING&&s.element_count==1){runtime=Runtime(s.elements[0].pid);assert(runtime.process_start_ticks==s.elements[0].process_start_ticks);return true;}return false;});Check(runtime);assert(kill(pid,SIGTERM)==0);Reap(pid);
    std::cout<<"PASS Supervisor -> membership -> Runtime -> Services -> descriptors -> direct dynamic Call\n";
}
}
extern "C" void* __real_memcpy(void*,const void*,std::size_t);
extern "C" void* __wrap_memcpy(void* dest,const void* source,std::size_t size){if(stop_copy&&size==sizeof(detail::ServiceRegistrySnapshot)){stop_copy=false;Send(copy_notice,1);raise(SIGSTOP);}return __real_memcpy(dest,source,size);}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--child"){Element e;ProcessRuntime r;r.SetLoopFrequency(500);return r.Run(e)?1:0;}alarm(120);Transport();FakeResponses();Lifecycle(0);Lifecycle(1);Lifecycle(2);Lifecycle(3);Malformed();Supervised(argv[0]);std::cout<<"R5 Service introspection/dynamic call PASS\n";}
