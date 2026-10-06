#include "kcf/introspection/detail/type_registry.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include <cstring>
#include <memory>
#include <mutex>
#include <unistd.h>
namespace kcf
{
bool ValidateTypeDescriptor(const TypeDescriptor& d) noexcept {
    if (d.protocol_version!=1 || d.struct_size!=sizeof(d) || !d.type_id ||
        !d.type_name[0] || !std::memchr(d.type_name,0,sizeof(d.type_name)) ||
        !d.payload_size || !d.payload_alignment || (d.payload_alignment & (d.payload_alignment-1)) ||
        d.payload_size % d.payload_alignment || !d.field_count || d.field_count>MAX_TYPE_FIELDS) return false;
    if (StableTypeId(d.type_name)!=d.type_id) return false;
    for(std::uint32_t i=0;i<d.field_count;++i) {
        const auto& f=d.fields[i];
        if (!f.name[0] || !std::memchr(f.name,0,sizeof(f.name)) || !PrimitiveSize(f.kind) ||
            f.element_size!=PrimitiveSize(f.kind) || !f.array_count || f.offset>d.payload_size) return false;
        const std::uint64_t bytes=std::uint64_t(f.element_size)*f.array_count;
        if(bytes>d.payload_size-f.offset) return false;
        for(std::uint32_t j=0;j<i;++j) {
            const auto& previous=d.fields[j];
            if(std::strcmp(f.name,previous.name)==0) return false;
            const auto end=std::uint64_t(previous.offset)+std::uint64_t(previous.element_size)*previous.array_count;
            if(f.offset<end && previous.offset<std::uint64_t(f.offset)+bytes) return false;
        }
    }
    return true;
}
}
namespace kcf::detail
{
namespace {
std::mutex mutex;
std::unique_ptr<SharedChannel<TypeRegistrySnapshot>> storage;
std::unique_ptr<TypeRegistrySnapshot> snapshot;
std::int32_t owner_pid=0;
std::uint64_t owner_ticks=0;
bool Equal(const TypeDescriptor& a,const TypeDescriptor& b) {
    if(a.type_id!=b.type_id || std::strcmp(a.type_name,b.type_name) || a.payload_size!=b.payload_size ||
       a.payload_alignment!=b.payload_alignment || a.field_count!=b.field_count) return false;
    for(std::uint32_t i=0;i<a.field_count;++i) {
        const auto& x=a.fields[i]; const auto& y=b.fields[i];
        if(std::strcmp(x.name,y.name) || x.kind!=y.kind || x.offset!=y.offset ||
           x.element_size!=y.element_size || x.array_count!=y.array_count) return false;
    }
    return true;
}
TypeDescriptor Normalize(const TypeDescriptor& source) {
    TypeDescriptor d{};
    d.type_id=source.type_id;std::strcpy(d.type_name,source.type_name);
    d.payload_size=source.payload_size;d.payload_alignment=source.payload_alignment;d.field_count=source.field_count;
    for(std::uint32_t i=0;i<d.field_count;++i) {
        const auto& f=source.fields[i];auto& out=d.fields[i];std::strcpy(out.name,f.name);
        out.kind=f.kind;out.offset=f.offset;out.element_size=f.element_size;out.array_count=f.array_count;
    }
    return d;
}
}
std::string TypeRegistryName(std::int32_t pid,std::uint64_t ticks) {
    return std::string("/")+TYPE_REGISTRY_PREFIX+std::to_string(pid)+"_"+std::to_string(ticks);
}
void BeginTypeRegistry(std::int32_t pid,std::uint64_t ticks) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if(storage) return;
        // No descriptor means no large SHM allocation for legacy runtimes.
        owner_pid=pid;owner_ticks=ticks;
    } catch (...) {}
}
std::uint64_t RegisterType(const TypeDescriptor& descriptor) noexcept {
    if(!ValidateTypeDescriptor(descriptor)) return 0;
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if(owner_pid!=getpid() || !owner_ticks) return 0;
        if(!storage) {
            auto values=std::make_unique<TypeRegistrySnapshot>();values->pid=owner_pid;values->process_start_ticks=owner_ticks;
            auto channel=std::make_unique<SharedChannel<TypeRegistrySnapshot>>();
            if(channel->TryCreate(TypeRegistryName(owner_pid,owner_ticks))!=0) return 0;
            // First registration is committed in the first publication below.
            snapshot=std::move(values);storage=std::move(channel);
        }
        for(std::uint32_t i=0;i<snapshot->type_count;++i)
            if(snapshot->types[i].type_id==descriptor.type_id)
                return Equal(snapshot->types[i],descriptor)?descriptor.type_id:0;
        if(snapshot->type_count>=MAX_TYPES) return 0;
        snapshot->types[snapshot->type_count++]=Normalize(descriptor);
        // TryPublishSnapshot has no post-commit notification failure. If pinned,
        // rollback the local append and leave existing published types untouched.
        if(storage->TryPublishSnapshot(*snapshot)!=0) {
            snapshot->types[--snapshot->type_count]={};return 0;
        }
        return descriptor.type_id;
    } catch (...) { return 0; }
}
void EndTypeRegistry() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if(owner_pid!=getpid()) return;
        if(storage) { (void)storage->Close();(void)storage->Unlink();storage.reset();snapshot.reset(); }
        owner_pid=0;owner_ticks=0;
    } catch (...) {}
}
}
