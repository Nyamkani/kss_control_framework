#ifdef NDEBUG
#undef NDEBUG
#endif
#include "r5_test_types.hpp"
#include "kcf/service/service_server.hpp"
#include "kcf/service/service_client.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/network/local_metadata.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
using namespace kcf;
using namespace std::chrono_literals;
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+6s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}}
int Count(const char* path){return std::distance(std::filesystem::directory_iterator(path),std::filesystem::directory_iterator{});}
void Run(int mode){
    detail::BeginRuntimeIntrospection(ExecutionMode::STANDALONE);detail::UpdateRuntimeIntrospection(ProcessState::RUNNING,0);
    RuntimeInfo self{};self.pid=getpid();assert(detail::ReadProcessIdentity(self.pid,self.process_start_ticks));
    if(mode!=4){assert(detail::RegisterType(TypeDescriptorTraits<R5Request>::Get()));assert(detail::RegisterType(TypeDescriptorTraits<R5Response>::Get()));}
    detail::ServiceRegistrationStatus invalid;
    assert(!detail::RegisterRecoverableService({},&invalid)&&invalid.failure==detail::ServiceRegistrationFailure::INVALID);
    int lock=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(lock>=0&&flock(lock,LOCK_EX|LOCK_NB)==0);
    ServiceServer server;int port=45000;while(server.Create(port)==-EADDRINUSE){assert(++port<46000);}
    assert((server.Register<R5Request,R5Response>(1,"/recovery/service",[](const auto& q,auto& r){r={q.x+q.y,7};})==0));
    assert(server.Start()==0);
    ServiceClient client;assert(client.Create(port)==0);R5Response response{};
    assert(client.Call(1,R5Request{2,3},response)==0&&response.x==5);
    ServiceInfo probe{};probe.port=port;probe.service_id=2;probe.request_size=8;probe.response_size=8;std::strcpy(probe.name,"/recovery/probe");
    detail::ServiceRegistrationStatus status;
    assert(detail::RegisterService(probe)==0); // Legacy API does not reserve a recovery token.
    auto token=detail::RegisterRecoverableService(probe,&status);assert(token&&status.initial_error==-EAGAIN&&status.pending&&!status.registration_id);
    assert(status.failure==detail::ServiceRegistrationFailure::STORAGE_CREATE);
    detail::ServiceRegistrationStatus duplicate;
    assert(!detail::RegisterRecoverableService(probe,&duplicate)&&duplicate.error==-EEXIST&&duplicate.failure==detail::ServiceRegistrationFailure::DUPLICATE);
    IntrospectionClient inspector;std::vector<ServiceInfo> services;assert(inspector.ListServices(self,services)==-ENOENT);
    network::MetadataConfig config;config.host.bytes[0]=1;config.boot_id.bytes[0]=2;config.runtime_pids={getpid()};config.remote_services={"/recovery/service"};
    std::vector<network::LocalEndpoint> endpoints;assert(network::CollectLocalMetadata(config,endpoints)==0&&endpoints.empty());
    if(mode==1)server.Stop();
    if(mode==2){detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);}
    if(mode==3){Until([&]{return detail::GetServiceRegistrationStatus(token,status)&&!status.pending;});assert(status.recovery_attempts==64&&status.error==-EAGAIN&&!status.recovered);server.Stop();}
    assert(flock(lock,LOCK_UN)==0);close(lock);
    if(mode==0||mode==1||mode==4){
        Until([&]{return detail::GetServiceRegistrationStatus(token,status)&&status.recovered;});
        assert(status.registration_id==token&&status.initial_error==-EAGAIN&&status.error==0&&status.recovery_attempts<=64);
        assert(inspector.ListServices(self,services)==0&&services.size()==(mode==1?1:2));
        detail::UnregisterService(token);
        if(mode!=1){
            assert(inspector.ListServices(self,services)==0&&services.size()==1);
            const auto info=services[0];assert(info.registration_id&&info.service_id==1);
            assert(network::CollectLocalMetadata(config,endpoints)==0);
            TypeDescriptor descriptor;
            assert(inspector.GetType(self,info.request_type_id,descriptor)==0);
            assert(inspector.GetType(self,info.response_type_id,descriptor)==0);
            assert(endpoints.size()==1&&endpoints[0].service_registration==info.registration_id);
            server.Stop();
        }
        assert(inspector.ListServices(self,services)==0&&services.empty());
    }else if(mode==2){
        assert(!detail::GetServiceRegistrationStatus(token,status));
        detail::BeginRuntimeIntrospection(ExecutionMode::STANDALONE);
        auto next=detail::RegisterRecoverableService(probe,&status);assert(next>token&&!status.error);
        server.Stop(); // An old server must not erase a new Runtime's registration.
        assert(inspector.ListServices(self,services)==0&&services.size()==1&&services[0].registration_id==next);
        detail::UnregisterService(next);
    }
    else detail::UnregisterService(token);
    client.Close();detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
    assert(inspector.ListServices(self,services)==-ENOENT);
    assert(!std::filesystem::exists("/dev/shm"+detail::ServiceRegistryName(self.pid,self.process_start_ticks)));
    std::cout<<"PASS real directory EAGAIN mode="<<mode<<" Start/callback/recovery/cancel/end/budget\n";
}
// Existing Service SHM plus missing Type SHM isolates Type recovery from Service creation.
void TypeRecovery(int mode){
    detail::BeginRuntimeIntrospection(ExecutionMode::STANDALONE);detail::UpdateRuntimeIntrospection(ProcessState::RUNNING,0);
    RuntimeInfo self{};self.pid=getpid();assert(detail::ReadProcessIdentity(self.pid,self.process_start_ticks));
    ServiceInfo info{};info.port=46000;info.service_id=7;info.request_size=info.response_size=8;std::strcpy(info.name,"/typed/probe");
    detail::ServiceRegistrationStatus legacy;
    auto seed=detail::RegisterTypedService(info,nullptr,nullptr,&legacy);assert(seed);
    assert(!legacy.request_type.supported&&!legacy.request_type.pending&&!legacy.response_type.supported&&!legacy.response_type.pending);
    detail::UnregisterService(seed);
    SharedChannel<detail::ServiceRegistrySnapshot> reader;assert(reader.Open(detail::ServiceRegistryName(self.pid,self.process_start_ticks))==0);
    int lock=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(lock>=0&&flock(lock,LOCK_EX|LOCK_NB)==0);
    auto request=TypeDescriptorTraits<R5Request>::Get(),response=TypeDescriptorTraits<R5Response>::Get();
    if(mode==5)request.struct_size=0; // permanent Request error, retryable Response
    if(mode==6)response.struct_size=0; // retryable Request, permanent Response
    detail::ServiceRegistrationStatus status;
    auto token=detail::RegisterTypedService(info,&request,&response,&status);assert(token&&status.registration_id==token&&!status.pending);
    assert(status.request_type.pending==(mode!=5)&&status.response_type.pending==(mode!=6));
    if(mode!=5)assert(status.request_type.initial.error==-EAGAIN);
    if(mode!=6)assert(status.response_type.initial.error==-EAGAIN);
    ServiceServer server;assert(server.Create(46001)==0);
    assert((server.Register<R5Request,R5Response>(1,"/typed/server",[](const auto& q,auto& r){r={q.x+q.y,1};})==0));
    assert(server.Start()==0);
    ServiceClient client;assert(client.Create(46001)==0);R5Response value{};assert(client.Call(1,R5Request{2,4},value)==0&&value.x==6);
    IntrospectionClient inspector;std::vector<ServiceInfo> services;
    assert(inspector.ListServices(self,services)==0&&services.size()==2);
    auto real_id=services[1].registration_id;
    assert(!services[0].request_type_id&&!services[0].response_type_id&&!services[1].request_type_id&&!services[1].response_type_id);
    detail::ServiceRegistrySnapshot before{};std::uint64_t seq;assert(reader.ReadLatestSnapshot(before,seq)==0);
    int pinned_fd=-1;unsigned char* pinned=nullptr;detail::ChannelLayout layout;
    if(mode==7){
        using Payload=detail::ServiceRegistrySnapshot;using Slot=detail::ChannelSlot<Payload>;
        assert(detail::ComputeChannelLayout(sizeof(Payload),alignof(Payload),1,layout));
        pinned_fd=shm_open(detail::ServiceRegistryName(self.pid,self.process_start_ticks).c_str(),O_RDWR,0);assert(pinned_fd>=0);
        pinned=static_cast<unsigned char*>(mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,pinned_fd,0));assert(pinned!=MAP_FAILED);
        for(unsigned i=0;i<3;++i)reinterpret_cast<Slot*>(pinned+layout.slots+i*layout.stride)->users.store(1);
    }
    if(mode==1){server.Stop();detail::UnregisterService(token);}
    if(mode==2)detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
    if(mode==3){
        Until([&]{return detail::GetServiceRegistrationStatus(token,status)&&!status.request_type.pending&&!status.response_type.pending;});
        assert(status.request_type.recovery_attempts==64&&status.response_type.recovery_attempts==64);
        assert(status.request_type.latest.error==-EAGAIN&&status.response_type.latest.error==-EAGAIN);
    }
    if(mode==4){ // Release and Stop overlap with management work; no delayed ghost after Stop returns.
        std::thread release([&]{assert(flock(lock,LOCK_UN)==0);close(lock);});
        server.Stop();detail::UnregisterService(token);release.join();
    }else{assert(flock(lock,LOCK_UN)==0);close(lock);}
    if(mode==0||mode==5||mode==6){
        Until([&]{return detail::GetServiceRegistrationStatus(token,status)&&!status.request_type.pending&&!status.response_type.pending;});
        assert(status.registration_id==token);
        assert(status.request_type.latest.type_id==(mode==5?0:request.type_id));
        assert(status.response_type.latest.type_id==(mode==6?0:response.type_id));
        if(mode==5)assert(status.request_type.latest.failure==detail::TypeRegistrationFailure::INVALID_DESCRIPTOR&&status.request_type.recovery_attempts==0);
        if(mode==6)assert(status.response_type.latest.failure==detail::TypeRegistrationFailure::INVALID_DESCRIPTOR&&status.response_type.recovery_attempts==0);
        Until([&]{assert(inspector.ListServices(self,services)==0);for(const auto& e:services)if(e.registration_id==real_id)return e.request_type_id&&e.response_type_id;return false;});
        detail::ServiceRegistrySnapshot after{};assert(reader.ReadLatestSnapshot(after,seq)==0&&after.revision>before.revision);
        assert(after.service_count==2&&after.services[0].registration_id==token&&after.services[1].registration_id==real_id);
        std::vector<TypeDescriptor> types;assert(inspector.ListTypes(self,types)==0&&types.size()==2);
        TypeDescriptor d;assert(inspector.GetType(self,TypeDescriptorTraits<R5Request>::Get().type_id,d)==0);
        assert(inspector.GetType(self,TypeDescriptorTraits<R5Response>::Get().type_id,d)==0);
        network::MetadataConfig config;config.host.bytes[0]=1;config.boot_id.bytes[0]=2;config.runtime_pids={getpid()};config.remote_services={"/typed/server"};
        std::vector<network::LocalEndpoint> endpoints;assert(network::CollectLocalMetadata(config,endpoints)==0);
        assert(std::count_if(endpoints.begin(),endpoints.end(),[&](const auto& e){return e.metadata.name=="/typed/server"&&e.service_registration==real_id;})==1);
        server.Stop();detail::UnregisterService(token);
    }else if(mode==2){assert(!detail::GetServiceRegistrationStatus(token,status));server.Stop();}
    else if(mode==3){
        assert(inspector.ListServices(self,services)==0&&services.size()==2&&!services[0].request_type_id&&!services[0].response_type_id);
        server.Stop();detail::UnregisterService(token);
    }
    if(mode==7){
        Until([&]{return detail::GetServiceRegistrationStatus(token,status)&&status.failure==detail::ServiceRegistrationFailure::PUBLISH;});
        assert(status.error==-EAGAIN&&!status.registration_id&&!status.request_type.pending&&!status.response_type.pending);
        assert(inspector.ListServices(self,services)==-ENOENT);
        server.Stop();detail::UnregisterService(token);
        assert(munmap(pinned,layout.length)==0);close(pinned_fd);
    }
    if(mode!=2&&mode!=7){assert(inspector.ListServices(self,services)==0&&services.empty());}
    client.Close();reader.Close();detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
    assert(!std::filesystem::exists("/dev/shm"+detail::ServiceRegistryName(self.pid,self.process_start_ticks)));
    std::cout<<"PASS Type EAGAIN -> same Service ID / revision / metadata mode="<<mode<<"\n";
}
int main(){
    detail::ServiceRegistrationStatus inactive;assert(!detail::RegisterRecoverableService({},&inactive)&&inactive.failure==detail::ServiceRegistrationFailure::INACTIVE);
    const auto fd=Count("/proc/self/fd"),threads=Count("/proc/self/task");
    for(int i=0;i<5;++i){Run(i);Until([&]{return Count("/proc/self/task")==threads;});assert(Count("/proc/self/fd")==fd);}
    for(int i=0;i<8;++i){TypeRecovery(i);Until([&]{return Count("/proc/self/task")==threads;});assert(Count("/proc/self/fd")==fd);}
    ServiceInfo bad{};detail::ServiceRegistrationStatus status;assert(!detail::RegisterRecoverableService(bad,&status)&&status.failure==detail::ServiceRegistrationFailure::STOPPING);
    std::cout<<"PASS Service recovery FD/worker teardown; snapshot ABI unchanged\n";
}
