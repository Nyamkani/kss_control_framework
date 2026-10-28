#include "kcf/introspection/detail/service_registry.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
namespace kcf::detail {
namespace {
std::mutex mutex;
std::unique_ptr<SharedChannel<ServiceRegistrySnapshot>> storage;
ServiceRegistrySnapshot snapshot;
std::uint64_t last_id=0;
struct Entry { std::uint64_t token{0}; ServiceInfo info{}; ServiceRegistrationStatus status{};
    std::unique_ptr<TypeDescriptor> request, response; };
std::array<Entry,MAX_SERVICES> entries;
std::condition_variable changed;
std::thread worker;
bool stopping=false;
bool Pending(const Entry& e){return e.status.pending||e.status.request_type.pending||e.status.response_type.pending;}
void CancelTypes(Entry& e){e.request.reset();e.response.reset();e.status.request_type.pending=false;e.status.response_type.pending=false;}
bool Active(){return snapshot.pid==getpid()&&snapshot.process_start_ticks;}
void Abandon(int error=-ESHUTDOWN){
    if(storage){(void)storage->Close();(void)storage->Unlink();storage.reset();}
    snapshot.service_count=0;
    // Do not resurrect an observational generation after snapshot failure.
    for(auto& e:entries) if(e.token){e.status.pending=false;e.status.registration_id=0;e.status.error=error;e.status.failure=ServiceRegistrationFailure::PUBLISH;CancelTypes(e);}
}
int Publish(){if(snapshot.revision==UINT64_MAX)return -EOVERFLOW;++snapshot.revision;return storage->TryPublishSnapshot(snapshot);}
int Commit(Entry& entry){
    auto& s=entry.status;
    if(!storage){
        auto candidate=std::make_unique<SharedChannel<ServiceRegistrySnapshot>>();
        int error=candidate->TryCreate(ServiceRegistryName(snapshot.pid,snapshot.process_start_ticks));
        if(error){s.failure=ServiceRegistrationFailure::STORAGE_CREATE;return error;}
        storage=std::move(candidate);
    }
    snapshot.services[snapshot.service_count++]=entry.info;
    const int error=Publish();
    if(error){Abandon(error);s.failure=ServiceRegistrationFailure::PUBLISH;return error;}
    s.registration_id=entry.token;s.failure=ServiceRegistrationFailure::NONE;return 0;
}
void InitializeType(const TypeDescriptor* descriptor, std::uint32_t size,
                    ServiceTypeRegistrationStatus& status, std::unique_ptr<TypeDescriptor>& retained,
                    std::uint64_t& id){
    id=0;
    if(!descriptor)return;
    status.supported=true;
    status.initial=status.latest=descriptor->payload_size==size?RegisterTypeDetailed(*descriptor):
        TypeRegistrationResult{0,-EINVAL,TypeRegistrationFailure::INVALID_DESCRIPTOR};
    id=status.latest.type_id;
    if(status.latest.Retryable()){
        try{retained=std::make_unique<TypeDescriptor>(*descriptor);status.pending=true;}
        catch(...){status.latest={0,-ENOMEM,TypeRegistrationFailure::RESOURCE};}
    }
}
bool RecoverType(ServiceTypeRegistrationStatus& status,std::unique_ptr<TypeDescriptor>& descriptor,std::uint64_t& id){
    if(!status.pending)return false;
    ++status.recovery_attempts;
    status.latest=RegisterTypeDetailed(*descriptor);
    const bool updated=status.latest.type_id!=0;
    if(updated)id=status.latest.type_id;
    status.pending=status.latest.Retryable()&&status.recovery_attempts<64;
    if(!status.pending)descriptor.reset();
    return updated;
}
void Recover(){
    std::unique_lock<std::mutex> lock(mutex);
    while(!stopping){
        changed.wait(lock,[]{return stopping||std::any_of(entries.begin(),entries.end(),Pending);});
        if(stopping||changed.wait_for(lock,std::chrono::milliseconds(50),[]{return stopping;}))break;
        for(auto& e:entries){
            if(!Pending(e))continue;
            if(!Active()){e.status.pending=false;e.status.error=-ESHUTDOWN;e.status.failure=ServiceRegistrationFailure::INACTIVE;CancelTypes(e);continue;}
            // Same lock order as Endpoint type recovery: registry -> Type Registry.
            // Neither path takes Runtime locks; End joins before EndTypeRegistry.
            const bool request=RecoverType(e.status.request_type,e.request,e.info.request_type_id);
            const bool response=RecoverType(e.status.response_type,e.response,e.info.response_type_id);
            if(e.status.pending){
                ++e.status.recovery_attempts;
                try{e.status.error=Commit(e);}catch(...){e.status.error=-ENOMEM;e.status.failure=ServiceRegistrationFailure::RESOURCE;}
                e.status.recovered=e.status.error==0;
                e.status.pending=e.status.error==-EAGAIN&&e.status.failure==ServiceRegistrationFailure::STORAGE_CREATE&&e.status.recovery_attempts<64;
                if(!e.status.pending&&!e.status.registration_id)CancelTypes(e);
            }else if((request||response)&&e.status.registration_id){
                auto end=snapshot.services+snapshot.service_count;
                auto found=std::find_if(snapshot.services,end,[&](const ServiceInfo& info){return info.registration_id==e.token;});
                if(found==end){CancelTypes(e);continue;}
                *found=e.info;
                const int error=Publish(); // Existing ID, exactly one revision per changed snapshot.
                if(error)Abandon(error);
            }
        }
    }
}
struct Cleanup {
    ~Cleanup(){
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;changed.notify_all();}
        if(worker.joinable())worker.join();
    }
};
std::uint64_t Register(ServiceInfo info,bool recover,ServiceRegistrationStatus* result, const TypeDescriptor* request=nullptr, const TypeDescriptor* response=nullptr, bool typed=false){
    ServiceRegistrationStatus status;
    const auto fail=[&](int error,ServiceRegistrationFailure reason){status.initial_error=status.error=error;status.failure=reason;if(result)*result=status;return std::uint64_t{0};};
    std::lock_guard<std::mutex> lock(mutex);
    if(stopping)return fail(-ESHUTDOWN,ServiceRegistrationFailure::STOPPING);
    if(!Active())return fail(-ENODEV,ServiceRegistrationFailure::INACTIVE);
    if(!info.port||!info.request_size||!info.response_size||!info.name[0]||
       !std::memchr(info.name,0,sizeof(info.name))||
       !std::memchr(info.diagnostic_request_type_name,0,sizeof(info.diagnostic_request_type_name))||
       !std::memchr(info.diagnostic_response_type_name,0,sizeof(info.diagnostic_response_type_name)))
        return fail(-EINVAL,ServiceRegistrationFailure::INVALID);
    for(const auto& e:entries)if(e.token&&e.info.port==info.port&&e.info.service_id==info.service_id)
        return fail(-EEXIST,ServiceRegistrationFailure::DUPLICATE);
    auto slot=std::find_if(entries.begin(),entries.end(),[](const Entry& e){return !e.token;});
    if(slot==entries.end()||last_id==UINT64_MAX)return fail(-ENOSPC,ServiceRegistrationFailure::CAPACITY);
    auto& e=*slot;e={};e.token=++last_id;info.registration_id=e.token;e.info=info;
    try{
        if(typed){
            InitializeType(request,info.request_size,e.status.request_type,e.request,e.info.request_type_id);
            InitializeType(response,info.response_size,e.status.response_type,e.response,e.info.response_type_id);
        }
        e.status.initial_error=e.status.error=Commit(e);
        if(recover&&e.status.error==-EAGAIN&&e.status.failure==ServiceRegistrationFailure::STORAGE_CREATE){
            e.status.pending=true;
        }
        if((!e.status.error||e.status.pending)&&Pending(e)){
            static Cleanup cleanup;
            if(!worker.joinable())worker=std::thread(Recover);
            changed.notify_all();
        }
    }catch(...){
        for(auto* type:{&e.status.request_type,&e.status.response_type})
            if(type->pending)type->latest={0,-ENOMEM,TypeRegistrationFailure::RESOURCE};
        CancelTypes(e);e.status.pending=false;
        // A committed Service must retain its unregister token even if worker creation fails.
        if(!e.status.registration_id){e.status.initial_error=e.status.error=-ENOMEM;e.status.failure=ServiceRegistrationFailure::RESOURCE;}
    }
    if(result)*result=e.status;
    if(e.status.error&&!e.status.pending){e={};return 0;}
    return e.token;
}
}
std::string ServiceRegistryName(std::int32_t pid,std::uint64_t ticks){return std::string("/")+SERVICE_REGISTRY_PREFIX+std::to_string(pid)+"_"+std::to_string(ticks);}
void BeginServiceRegistry(std::int32_t pid,std::uint64_t ticks) noexcept {
    try{std::lock_guard<std::mutex> lock(mutex);if(Active())return;snapshot={};snapshot.pid=pid;snapshot.process_start_ticks=ticks;stopping=false;}catch(...){}
}
void EndServiceRegistry() noexcept {
    try{
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;changed.notify_all();}
        // Never join with registry mutex held. Worker never calls Runtime or ServiceServer.
        if(worker.joinable())worker.join();
        std::lock_guard<std::mutex> lock(mutex);Abandon();snapshot={};for(auto& e:entries)e={};
    }catch(...){}
}
std::uint64_t RegisterService(ServiceInfo info) noexcept {
    try{return Register(info,false,nullptr);}catch(...){return 0;}
}
std::uint64_t RegisterRecoverableService(ServiceInfo info,ServiceRegistrationStatus* result) noexcept {
    try{return Register(info,true,result);}catch(...){if(result){*result={};result->initial_error=result->error=-ENOMEM;result->failure=ServiceRegistrationFailure::RESOURCE;}return 0;}
}
std::uint64_t RegisterTypedService(ServiceInfo info,const TypeDescriptor* request,const TypeDescriptor* response,ServiceRegistrationStatus* result) noexcept {
    try{return Register(info,true,result,request,response,true);}catch(...){if(result){*result={};result->initial_error=result->error=-ENOMEM;result->failure=ServiceRegistrationFailure::RESOURCE;}return 0;}
}
bool GetServiceRegistrationStatus(std::uint64_t token,ServiceRegistrationStatus& status) noexcept {
    try{std::lock_guard<std::mutex> lock(mutex);for(const auto& e:entries)if(token&&e.token==token){status=e.status;return true;}}catch(...){}return false;
}
void UnregisterService(std::uint64_t id) noexcept {
    if(!id)return;
    try{std::lock_guard<std::mutex> lock(mutex);
        for(auto& e:entries)if(e.token==id)e={};
        if(!Active()||!storage)return;
        for(std::uint32_t i=0;i<snapshot.service_count;++i)if(snapshot.services[i].registration_id==id){
            snapshot.services[i]=snapshot.services[--snapshot.service_count];snapshot.services[snapshot.service_count]={};
            const int error=Publish();if(error)Abandon(error);return;
        }
    }catch(...){}
}
}
