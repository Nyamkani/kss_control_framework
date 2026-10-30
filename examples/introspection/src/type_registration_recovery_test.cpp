#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/network/local_metadata.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <iostream>
#include <sys/file.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

struct Value { std::uint64_t count; };
namespace kcf {
template<> struct TypeDescriptorTraits<Value> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() {
        auto d=MakeTypeDescriptor<Value>("kcf.recovery.Value.v1");
        d.field_count=1;d.fields[0]=MakeField<Value,std::uint64_t>("count",0);return d;
    }
};
}
// Actual directory lock contention, installed immediately before TryCreate's
// nonblocking flock. Publisher's earlier blocking storage creation is unaffected.
extern "C" int __real_flock(int,int);
std::atomic<bool> arm{false};
int held=-1;
extern "C" int __wrap_flock(int fd,int operation) {
    if((operation&LOCK_NB) && arm.exchange(false)) {
        held=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(held>=0);
        assert(__real_flock(held,LOCK_EX|LOCK_NB)==0);
    }
    return __real_flock(fd,operation);
}
using namespace kcf;
using namespace std::chrono_literals;
void Release(){assert(held>=0);assert(__real_flock(held,LOCK_UN)==0);close(held);held=-1;}
template<class F> void Until(F f) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while(!f()) {assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
RuntimeInfo Self() {RuntimeInfo r{};r.pid=getpid();assert(detail::ReadProcessIdentity(r.pid,r.process_start_ticks));return r;}
EndpointInfo Endpoint(const RuntimeInfo& r) {
    IntrospectionClient c;std::vector<EndpointInfo> all;assert(c.ListEndpoints(r,all)==0&&all.size()==1);return all[0];
}
void Run(ExecutionMode mode,int disposition) {
    detail::BeginRuntimeIntrospection(mode);detail::UpdateRuntimeIntrospection(ProcessState::RUNNING,0);
    const auto runtime=Self();const auto name="/kcf_type_recovery_"+std::to_string(getpid());
    Publisher<Value> publisher;arm=true;assert(publisher.Create(name)==0);assert(!arm&&held>=0);
    const auto before=Endpoint(runtime);assert(before.type_id==0&&before.registration_id);
    detail::EndpointTypeRegistrationStatus status;
    assert(detail::GetEndpointTypeRegistrationStatus(before.registration_id,status));
    assert(status.initial.error==-EAGAIN&&status.initial.failure==detail::TypeRegistrationFailure::STORAGE_CREATE&&status.pending);
    // Local traffic remains operational while type storage cannot be created.
    assert(publisher.Publish({42})==0);
    SharedChannel<Value> reader;assert(reader.Open(name)==0);Value v{};TopicReadInfo read_info;assert(reader.ReadLatest(v,read_info)==0&&v.count==42);reader.Close();
    network::MetadataConfig config;config.runtime_pids={getpid()};config.host.bytes[0]=1;config.boot_id.bytes[0]=2;
    config.remote_topics={name};std::vector<network::LocalEndpoint> endpoints;
    assert(network::CollectLocalMetadata(config,endpoints)==0&&endpoints.empty());
    if(disposition==3) {
        Until([&]{return detail::GetEndpointTypeRegistrationStatus(before.registration_id,status)&&!status.pending;});
        assert(status.recovery_attempts==64&&!status.recovered&&status.latest.error==-EAGAIN);
        Release();publisher.Close();
    } else if(disposition==1) {
        auto descriptor=TypeDescriptorTraits<Value>::Get();
        const auto survivor=detail::RegisterEndpointWithType(EndpointKind::TOPIC,EndpointRole::PUBLISHER,
            "/close_survivor",sizeof(Value),"fixture",&descriptor);assert(survivor);
        publisher.Close();Release();
        Until([&]{return detail::GetEndpointTypeRegistrationStatus(survivor,status)&&status.recovered;});
        detail::UnregisterEndpoint(survivor);
        assert(!detail::GetEndpointTypeRegistrationStatus(before.registration_id,status));
        IntrospectionClient c;std::vector<EndpointInfo> all;assert(c.ListEndpoints(runtime,all)==0&&all.empty());
    } else if(disposition==2) {
        // Shutdown must not wait for this external lock or resurrect anything.
        detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);Release();
        assert(!detail::GetEndpointTypeRegistrationStatus(before.registration_id,status));publisher.Close();
    } else {
        Release();Until([&]{return detail::GetEndpointTypeRegistrationStatus(before.registration_id,status)&&status.recovered;});
        const auto after=Endpoint(runtime);assert(after.registration_id==before.registration_id);
        assert(after.type_id==TypeDescriptorTraits<Value>::Get().type_id);
        assert(status.initial.error==-EAGAIN&&status.latest.error==0&&!status.pending&&status.recovery_attempts<=64);
        IntrospectionClient c;TypeDescriptor descriptor;assert(c.GetType(runtime,after.type_id,descriptor)==0);
        assert(network::CollectLocalMetadata(config,endpoints)==0&&endpoints.size()==1);
        publisher.Close();
    }
    publisher.Unlink();detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
    std::cout<<"PASS real TryCreate EAGAIN mode="<<int(mode)<<" disposition="<<disposition<<"\n";
}
int Count(const char* path) {
    DIR* dir=opendir(path);assert(dir);int n=0;
    while(auto e=readdir(dir)) if(e->d_name[0]!='.') ++n;
    closedir(dir);return n;
}
void PinnedRecovery() {
    detail::BeginRuntimeIntrospection(ExecutionMode::STANDALONE);
    const auto runtime=Self();auto d=TypeDescriptorTraits<Value>::Get();
    assert(detail::RegisterType(d));
    const auto name=detail::TypeRegistryName(runtime.pid,runtime.process_start_ticks);
    const int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);
    using Payload=detail::TypeRegistrySnapshot;
    using Slot=detail::ChannelSlot<Payload>;
    detail::ChannelLayout layout;assert(detail::ComputeChannelLayout(sizeof(Payload),alignof(Payload),1,layout));
    auto raw=static_cast<unsigned char*>(mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
    assert(raw!=MAP_FAILED);close(fd);
    auto slot=[&](unsigned i)->Slot& {return *reinterpret_cast<Slot*>(raw+layout.slots+i*layout.stride);};
    for(unsigned i=0;i<3;++i)slot(i).users.fetch_add(1);
    std::strcpy(d.type_name,"kcf.recovery.Pinned.v1");d.type_id=StableTypeId(d.type_name);
    const auto id=detail::RegisterEndpointWithType(EndpointKind::TOPIC,EndpointRole::PUBLISHER,
        "/pinned_descriptor",sizeof(Value),"fixture",&d);assert(id);
    detail::EndpointTypeRegistrationStatus status;
    assert(detail::GetEndpointTypeRegistrationStatus(id,status)&&status.pending);
    assert(status.initial.error==-EAGAIN&&status.initial.failure==detail::TypeRegistrationFailure::SNAPSHOT_PUBLISH);
    for(unsigned i=0;i<3;++i)slot(i).users.fetch_sub(1);
    Until([&]{return detail::GetEndpointTypeRegistrationStatus(id,status)&&status.recovered;});
    assert(Endpoint(runtime).registration_id==id&&Endpoint(runtime).type_id==d.type_id);
    assert(munmap(raw,layout.length)==0);detail::UnregisterEndpoint(id);
    detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
    std::cout<<"PASS pinned snapshot publish failure recovery\n";
}
void PermanentFailures() {
    detail::BeginRuntimeIntrospection(ExecutionMode::STANDALONE);
    auto d=TypeDescriptorTraits<Value>::Get();
    assert(detail::RegisterTypeDetailed(d).type_id==d.type_id);
    auto collision=d;collision.fields[0].kind=FieldValueKind::INT64;
    assert(detail::RegisterTypeDetailed(collision).failure==detail::TypeRegistrationFailure::SCHEMA_CONFLICT);
    for(unsigned i=1;i<detail::MAX_TYPES;++i) {
        auto name="kcf.recovery.capacity."+std::to_string(i);
        std::memset(d.type_name,0,sizeof(d.type_name));std::strcpy(d.type_name,name.c_str());d.type_id=StableTypeId(name);
        assert(detail::RegisterTypeDetailed(d).type_id==d.type_id);
    }
    std::strcpy(d.type_name,"kcf.recovery.overflow");d.type_id=StableTypeId(d.type_name);
    const auto result=detail::RegisterTypeDetailed(d);
    assert(result.error==-ENOSPC&&result.failure==detail::TypeRegistrationFailure::CAPACITY&&!result.Retryable());
    const auto id=detail::RegisterEndpointWithType(EndpointKind::TOPIC,EndpointRole::PUBLISHER,
        "/capacity_endpoint",sizeof(Value),"fixture",&d);
    detail::EndpointTypeRegistrationStatus status;
    assert(id&&detail::GetEndpointTypeRegistrationStatus(id,status)&&!status.pending);
    assert(status.initial.failure==detail::TypeRegistrationFailure::CAPACITY&&status.recovery_attempts==0);
    detail::UnregisterEndpoint(id);
    d.type_id=0;
    const auto bad=detail::RegisterEndpointWithType(EndpointKind::TOPIC,EndpointRole::PUBLISHER,
        "/invalid_endpoint",sizeof(Value),"fixture",&d);
    assert(bad&&detail::GetEndpointTypeRegistrationStatus(bad,status)&&!status.pending);
    assert(status.initial.failure==detail::TypeRegistrationFailure::INVALID_DESCRIPTOR&&status.recovery_attempts==0);
    detail::UnregisterEndpoint(bad);
    detail::EndRuntimeIntrospection(ProcessState::STOPPED,0);
}
int main() {
    alarm(30);
    const int fds=Count("/proc/self/fd"),threads=Count("/proc/self/task"),shm=Count("/dev/shm");
    auto d=TypeDescriptorTraits<Value>::Get();
    assert(detail::RegisterTypeDetailed(d).failure==detail::TypeRegistrationFailure::INACTIVE);
    d.type_id=0;assert(detail::RegisterTypeDetailed(d).failure==detail::TypeRegistrationFailure::INVALID_DESCRIPTOR);
    for(auto mode:{ExecutionMode::STANDALONE,ExecutionMode::SUPERVISED}) for(int i=0;i<3;++i)Run(mode,i);
    Run(ExecutionMode::STANDALONE,3);PinnedRecovery();PermanentFailures();
    assert(Count("/proc/self/fd")==fds&&Count("/proc/self/task")==threads&&Count("/dev/shm")==shm);
    std::cout<<"type registration recovery PASS\n";
}
