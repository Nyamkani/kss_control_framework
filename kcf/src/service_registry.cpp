#include "kcf/introspection/detail/service_registry.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <memory>
#include <mutex>
#include <limits>
#include <unistd.h>
namespace kcf::detail {
namespace {
std::mutex mutex;
std::unique_ptr<SharedChannel<ServiceRegistrySnapshot>> storage;
ServiceRegistrySnapshot snapshot;
std::uint64_t last_id=0;
bool Active(){return snapshot.pid==getpid()&&snapshot.process_start_ticks;}
void Abandon(){if(storage){(void)storage->Close();(void)storage->Unlink();storage.reset();}snapshot.service_count=0;}
bool Publish(){if(snapshot.revision==UINT64_MAX)return false;++snapshot.revision;return storage->TryPublishSnapshot(snapshot)==0;}
}
std::string ServiceRegistryName(std::int32_t pid,std::uint64_t ticks){return std::string("/")+SERVICE_REGISTRY_PREFIX+std::to_string(pid)+"_"+std::to_string(ticks);}
void BeginServiceRegistry(std::int32_t pid,std::uint64_t ticks) noexcept {
    try{std::lock_guard<std::mutex> lock(mutex);if(Active())return;snapshot={};snapshot.pid=pid;snapshot.process_start_ticks=ticks;}catch(...){}
}
void EndServiceRegistry() noexcept {
    try{std::lock_guard<std::mutex> lock(mutex);if(!Active())return;Abandon();snapshot={};}catch(...){}
}
std::uint64_t RegisterService(ServiceInfo info) noexcept {
    try{
        std::lock_guard<std::mutex> lock(mutex);
        if(!Active()||snapshot.service_count>=MAX_SERVICES||last_id==UINT64_MAX)return 0;
        if(!storage){auto candidate=std::make_unique<SharedChannel<ServiceRegistrySnapshot>>();
            if(candidate->TryCreate(ServiceRegistryName(snapshot.pid,snapshot.process_start_ticks)))return 0;
            storage=std::move(candidate);}
        info.registration_id=++last_id;snapshot.services[snapshot.service_count++]=info;
        if(!Publish()){Abandon();return 0;}return info.registration_id;
    }catch(...){return 0;}
}
void UnregisterService(std::uint64_t id) noexcept {
    if(!id)return;
    try{std::lock_guard<std::mutex> lock(mutex);if(!Active()||!storage)return;
        for(std::uint32_t i=0;i<snapshot.service_count;++i)if(snapshot.services[i].registration_id==id){
            snapshot.services[i]=snapshot.services[--snapshot.service_count];snapshot.services[snapshot.service_count]={};
            if(!Publish())Abandon();
            return;
        }
    }catch(...){}
}
}
