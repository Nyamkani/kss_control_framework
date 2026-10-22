#include "kcf/introspection/detail/action_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include "kcf/service/service_protocol.hpp"
#include <cstring>
#include <mutex>
#include <memory>
#include <vector>
#include <algorithm>
#include <unistd.h>
namespace kcf::detail {
namespace {
std::mutex mutex;std::unique_ptr<SharedChannel<ActionRegistrySnapshot>> storage;
std::int32_t pid=0;std::uint64_t ticks=0,last=0;
std::vector<ActionInfo> entries;std::vector<std::uint16_t> active;
void Publish(){
    if(!storage){auto s=std::make_unique<SharedChannel<ActionRegistrySnapshot>>();if(s->TryCreate(ActionRegistryName(pid,ticks)))return;storage=std::move(s);}
    ActionRegistrySnapshot snapshot;snapshot.pid=pid;snapshot.start_ticks=ticks;
    for(const auto& e:entries)if(std::find(active.begin(),active.end(),e.port)!=active.end())snapshot.actions[snapshot.count++]=e;
    if(storage->TryPublishSnapshot(snapshot)){storage->Close();storage->Unlink();storage.reset();}
}
}
std::string ActionRegistryName(std::int32_t p,std::uint64_t t){return std::string("/")+ACTION_REGISTRY_PREFIX+std::to_string(p)+"_"+std::to_string(t);}
void BeginActionRegistry(std::int32_t p,std::uint64_t t) noexcept {try{std::lock_guard<std::mutex> lock(mutex);pid=p;ticks=t;entries.clear();active.clear();}catch(...) {}}
void EndActionRegistry() noexcept {try{std::lock_guard<std::mutex> lock(mutex);if(storage){storage->Close();storage->Unlink();storage.reset();}pid=0;ticks=0;entries.clear();active.clear();}catch(...) {}}
std::uint64_t RegisterAction(ActionInfo info) noexcept {try{std::lock_guard<std::mutex> lock(mutex);
    if(info.name[0]!='/'||!std::memchr(info.name,0,sizeof(info.name))||!info.port||!info.ids.Valid())return 0;
    if(pid!=getpid()||!ticks||entries.size()==MAX_ACTIONS||last==UINT64_MAX||!info.goal_type_id||!info.feedback_type_id||!info.result_type_id)return 0;
    info.registration_id=++last;entries.push_back(info);Publish();return info.registration_id;
}catch(...){return 0;}}
void UnregisterAction(std::uint64_t id) noexcept {if(!id)return;try{std::lock_guard<std::mutex> lock(mutex);entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto& e){return e.registration_id==id;}),entries.end());if(pid==getpid()&&ticks)Publish();}catch(...) {}}
void SetActionPortActive(std::uint16_t port,bool on) noexcept {try{std::lock_guard<std::mutex> lock(mutex);if(pid!=getpid()||!ticks)return;
    active.erase(std::remove(active.begin(),active.end(),port),active.end());if(on)active.push_back(port);if(!entries.empty())Publish();
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
