#include "kcf/introspection/detail/action_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include "kcf/service/service_protocol.hpp"
#include <cstring>
#include <array>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>
#include <unistd.h>
namespace kcf::detail {
namespace {
std::mutex mutex;std::unique_ptr<SharedChannel<ActionRegistrySnapshot>> storage;
std::int32_t pid=0;std::uint64_t ticks=0,last=0;
struct Entry {
    ActionInfo info;
    std::array<std::unique_ptr<TypeDescriptor>,3> pending;
    unsigned attempts{0};
};
std::vector<Entry> entries;std::vector<std::uint16_t> active;
std::condition_variable changed;std::thread worker;
bool stopping=false, dirty=false;unsigned publish_attempts=0;
bool Pending(const Entry& e){return std::any_of(e.pending.begin(),e.pending.end(),[](const auto& d){return bool(d);});}
bool Complete(const Entry& e){return e.info.goal_type_id&&e.info.feedback_type_id&&e.info.result_type_id;}
int Publish(){
    if(!storage){auto s=std::make_unique<SharedChannel<ActionRegistrySnapshot>>();const int code=s->TryCreate(ActionRegistryName(pid,ticks));if(code)return code;storage=std::move(s);}
    ActionRegistrySnapshot snapshot;snapshot.pid=pid;snapshot.start_ticks=ticks;
    for(const auto& e:entries)if(Complete(e)&&std::find(active.begin(),active.end(),e.info.port)!=active.end())snapshot.actions[snapshot.count++]=e.info;
    const int code=storage->TryPublishSnapshot(snapshot);
    // Preserve the last complete snapshot on transient reader contention.
    if(code && code!=-EAGAIN){storage->Close();storage->Unlink();storage.reset();}
    return code;
}
void Recover(){
    std::unique_lock<std::mutex> lock(mutex);
    while(!stopping){
        changed.wait(lock,[]{return stopping||dirty||std::any_of(entries.begin(),entries.end(),Pending);});
        if(stopping||changed.wait_for(lock,std::chrono::milliseconds(50),[]{return stopping;}))break;
        if(pid!=getpid()||!ticks)break;
        bool updated=false;
        for(auto& e:entries)if(Pending(e)){
            ++e.attempts;
            std::uint64_t* ids[]{&e.info.goal_type_id,&e.info.feedback_type_id,&e.info.result_type_id};
            for(unsigned i=0;i<3;++i)if(e.pending[i]){
                const auto result=RegisterTypeDetailed(*e.pending[i]);
                if(result.type_id){*ids[i]=result.type_id;updated=true;}
                if(!result.Retryable()||e.attempts>=64)e.pending[i].reset();
            }
        }
        if(updated){dirty=true;publish_attempts=0;}
        if(dirty){
            ++publish_attempts;
            try{dirty=Publish()==-EAGAIN&&publish_attempts<64;}
            catch(...){dirty=false;}
        }
    }
}
struct Cleanup {~Cleanup(){EndActionRegistry();}};
void Update(){
    publish_attempts=0;dirty=Publish()==-EAGAIN;
    if(dirty||std::any_of(entries.begin(),entries.end(),Pending)){
        static Cleanup cleanup;
        if(!worker.joinable())worker=std::thread(Recover);
        changed.notify_all();
    }
}
std::uint64_t Register(ActionInfo info,const TypeDescriptor* const* types){
    std::lock_guard<std::mutex> lock(mutex);
    if(stopping||info.name[0]!='/'||!std::memchr(info.name,0,sizeof(info.name))||!info.port||!info.ids.Valid())return 0;
    if(pid!=getpid()||!ticks||entries.size()==MAX_ACTIONS||last==UINT64_MAX)return 0;
    Entry entry;entry.info=info;
    std::uint64_t* ids[]{&entry.info.goal_type_id,&entry.info.feedback_type_id,&entry.info.result_type_id};
    if(types)for(unsigned i=0;i<3;++i){
        if(!types[i])return 0;
        const auto result=RegisterTypeDetailed(*types[i]);*ids[i]=result.type_id;
        if(result.Retryable())entry.pending[i]=std::make_unique<TypeDescriptor>(*types[i]);
        else if(!result.type_id)return 0;
    }
    if(!Complete(entry)&&!Pending(entry))return 0;
    const auto id=entry.info.registration_id=++last;entries.push_back(std::move(entry));
    // A pending registration owns its ID immediately, so Close can cancel it.
    try{Update();}catch(...){} // Observational failure must not orphan the token.
    return id;
}
}
std::string ActionRegistryName(std::int32_t p,std::uint64_t t){return std::string("/")+ACTION_REGISTRY_PREFIX+std::to_string(p)+"_"+std::to_string(t);}
void BeginActionRegistry(std::int32_t p,std::uint64_t t) noexcept {EndActionRegistry();try{std::lock_guard<std::mutex> lock(mutex);pid=p;ticks=t;stopping=false;}catch(...) {}}
void EndActionRegistry() noexcept {try{
    {std::lock_guard<std::mutex> lock(mutex);stopping=true;changed.notify_all();}
    if(worker.joinable())worker.join();
    std::lock_guard<std::mutex> lock(mutex);
    if(storage){storage->Close();storage->Unlink();storage.reset();}
    pid=0;ticks=0;entries.clear();active.clear();dirty=false;
}catch(...) {}}
std::uint64_t RegisterAction(ActionInfo info) noexcept {try{return Register(info,nullptr);}catch(...){return 0;}}
std::uint64_t RegisterActionWithTypes(ActionInfo info,const TypeDescriptor* goal,const TypeDescriptor* feedback,const TypeDescriptor* result) noexcept {
    try{const TypeDescriptor* types[]{goal,feedback,result};return Register(info,types);}catch(...){return 0;}
}
void UnregisterAction(std::uint64_t id) noexcept {if(!id)return;try{std::lock_guard<std::mutex> lock(mutex);entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto& e){return e.info.registration_id==id;}),entries.end());if(!stopping&&pid==getpid()&&ticks)Update();}catch(...) {}}
void SetActionPortActive(std::uint16_t port,bool on) noexcept {try{std::lock_guard<std::mutex> lock(mutex);if(stopping||pid!=getpid()||!ticks)return;
    active.erase(std::remove(active.begin(),active.end(),port),active.end());if(on)active.push_back(port);if(!entries.empty())Update();
}catch(...) {}}
}

namespace kcf {
int IntrospectionClient::ListActions(const RuntimeInfo& runtime,std::vector<ActionInfo>& out){try{
    std::uint64_t ticks=0;if(!detail::ReadProcessIdentity(runtime.pid,ticks)||ticks!=runtime.process_start_ticks)return -ENOENT;
    SharedChannel<detail::ActionRegistrySnapshot> storage;int code=storage.Open(detail::ActionRegistryName(runtime.pid,ticks));if(code)return code;
    detail::ActionRegistrySnapshot snapshot;std::uint64_t sequence=0;code=storage.ReadLatestSnapshot(snapshot,sequence);if(code)return code;
    if(snapshot.protocol_version!=1||snapshot.struct_size!=sizeof(snapshot)||snapshot.pid!=runtime.pid||snapshot.start_ticks!=ticks||snapshot.count>detail::MAX_ACTIONS)return -EPROTO;
    std::vector<ActionInfo> entries;
    for(std::uint32_t i=0;i<snapshot.count;++i){const auto& e=snapshot.actions[i];
        if(!e.registration_id||!e.port||!e.ids.Valid()||e.name[0]!='/'||!std::memchr(e.name,0,sizeof(e.name))||!e.goal_type_id||!e.feedback_type_id||!e.result_type_id)return -EPROTO;
        if(!e.goal_request_size||e.goal_request_size>SERVICE_MAX_PAYLOAD||!e.result_response_size||e.result_response_size>SERVICE_MAX_PAYLOAD||!e.status_size||e.status_size>1024*1024||!e.status_alignment||(e.status_alignment&(e.status_alignment-1))||e.status_alignment>4096)return -EPROTO;
        if(e.goal_offset<8||e.goal_offset>=e.goal_request_size||e.feedback_offset<sizeof(ActionHeader)||e.feedback_offset>=e.status_size||e.result_offset<16||e.result_offset>=e.result_response_size)return -EPROTO;
        for(const auto& old:entries)if(old.registration_id==e.registration_id)return -EPROTO;
        entries.push_back(e);
    }
    if(!detail::ReadProcessIdentity(runtime.pid,ticks)||ticks!=runtime.process_start_ticks)return -ENOENT;
    out=std::move(entries);return 0;
}catch(const std::bad_alloc&){return -ENOMEM;}catch(...){return -EIO;}}
}
