#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/detail/supervisor_registry.hpp"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <new>

namespace kcf
{
int IntrospectionClient::ListRuntimes(std::vector<RuntimeInfo>& runtimes)
{
    try
    {
        const auto close_directory = [](DIR* value) { (void)closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> directory(opendir("/dev/shm"), close_directory);
        if (!directory) return -errno;
        std::vector<RuntimeInfo> found;
        for (;;)
        {
            errno = 0;
            const auto* entry = readdir(directory.get());
            if (!entry)
            {
                if (errno) return -errno;
                break;
            }
            std::int32_t pid = 0;
            std::uint64_t expected = 0, actual = 0;
            if (!detail::ParseRuntimeName(entry->d_name, pid, expected) ||
                !detail::ReadProcessIdentity(pid, actual) || actual != expected) continue;
            SharedParameter<RuntimeInfo> storage;
            int result = storage.Open(detail::RuntimeRegistryName(pid, expected));
            if (result == -ENOMEM) return result;
            if (result != 0) continue;
            RuntimeInfo info{};
            result = storage.Get(info);
            (void)storage.Close();
            if (result == -ENOMEM) return result;
            if (result != 0 || info.pid != pid || info.process_start_ticks != expected ||
                info.protocol_version != 1 || info.struct_size != sizeof(RuntimeInfo) ||
                (info.execution_mode != ExecutionMode::STANDALONE && info.execution_mode != ExecutionMode::SUPERVISED) ||
                (info.state != ProcessState::STOPPED && info.state != ProcessState::STARTING &&
                 info.state != ProcessState::RUNNING && info.state != ProcessState::STOPPING && info.state != ProcessState::ERROR) ||
                !std::memchr(info.executable, '\0', sizeof(info.executable))) continue;
            // Recheck after copying: exit and PID reuse during discovery are normal.
            if (!detail::ReadProcessIdentity(pid, actual) || actual != expected) continue;
            found.push_back(info);
        }
        runtimes.swap(found);
        return 0;
    }
    catch (const std::bad_alloc&) { return -ENOMEM; }
    catch (...) { return -EIO; }
}
}

namespace kcf
{
int IntrospectionClient::ListEndpoints(const RuntimeInfo& runtime, std::vector<EndpointInfo>& endpoints)
{
    try
    {
        std::uint64_t ticks = 0;
        if (runtime.pid <= 0 || !runtime.process_start_ticks) return -EINVAL;
        if (!detail::ReadProcessIdentity(runtime.pid, ticks) || ticks != runtime.process_start_ticks) return -ENOENT;
        SharedParameter<detail::EndpointRegistrySnapshot> storage;
        int result = storage.Open(detail::EndpointRegistryName(runtime.pid, ticks));
        if (result != 0) return result;
        detail::EndpointRegistrySnapshot snapshot{};
        result = storage.Get(snapshot);
        (void)storage.Close();
        if (result != 0) return result;
        if (snapshot.protocol_version != 2 || snapshot.struct_size != sizeof(snapshot) ||
            snapshot.pid != runtime.pid || snapshot.process_start_ticks != runtime.process_start_ticks ||
            snapshot.endpoint_count > detail::MAX_ENDPOINTS) return -EPROTO;
        std::vector<EndpointInfo> found;
        found.reserve(snapshot.endpoint_count);
        for (std::uint32_t i = 0; i < snapshot.endpoint_count; ++i)
        {
            const auto& entry = snapshot.endpoints[i];
            const bool role_valid =
                (entry.kind == EndpointKind::TOPIC && (entry.role == EndpointRole::PUBLISHER || entry.role == EndpointRole::SUBSCRIBER)) ||
                (entry.kind == EndpointKind::PARAMETER && (entry.role == EndpointRole::PARAMETER_OWNER || entry.role == EndpointRole::PARAMETER_CLIENT));
            if (!role_valid || !entry.registration_id || !entry.payload_size || !entry.name[0] ||
                !std::memchr(entry.name, 0, sizeof(entry.name)) ||
                !std::memchr(entry.diagnostic_type_name, 0, sizeof(entry.diagnostic_type_name))) return -EPROTO;
            for (const auto& previous : found)
                if (previous.registration_id == entry.registration_id) return -EPROTO;
            found.push_back(entry);
        }
        if (!detail::ReadProcessIdentity(runtime.pid, ticks) || ticks != runtime.process_start_ticks) return -ENOENT;
        endpoints.swap(found);
        return 0;
    }
    catch (const std::bad_alloc&) { return -ENOMEM; }
    catch (...) { return -EIO; }
}
}

namespace kcf
{
int IntrospectionClient::ListSupervisors(std::vector<SupervisorInfo>& supervisors)
{
    try
    {
        const auto close_directory = [](DIR* value) { (void)closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> directory(opendir("/dev/shm"), close_directory);
        if (!directory) return -errno;
        std::vector<SupervisorInfo> found;
        for (;;)
        {
            errno = 0;
            const auto* entry = readdir(directory.get());
            if (!entry) { if (errno) return -errno; break; }
            std::int32_t pid = 0; std::uint64_t expected = 0, actual = 0;
            if (!detail::ParseDiscoveryName(entry->d_name, detail::SUPERVISOR_REGISTRY_PREFIX, pid, expected) ||
                !detail::ReadProcessIdentity(pid, actual) || actual != expected) continue;
            SharedChannel<SupervisorInfo> storage;
            int result = storage.Open(detail::SupervisorRegistryName(pid, expected));
            if (result == -ENOMEM) return result;
            if (result != 0) continue;
            SupervisorInfo info{}; std::uint32_t sequence = 0;
            result = storage.ReadLatestSnapshot(info, sequence);
            (void)storage.Close();
            if (result != 0 || info.pid != pid || info.process_start_ticks != expected ||
                info.protocol_version != 1 || info.struct_size != sizeof(info) ||
                info.element_count > MAX_SUPERVISOR_ELEMENTS ||
                !std::memchr(info.application_name, 0, sizeof(info.application_name))) continue;
            switch (info.application_state)
            {
            case ApplicationState::INITIALIZING: case ApplicationState::RUNNING:
            case ApplicationState::ERROR: case ApplicationState::RESETTING:
            case ApplicationState::SHUTTING_DOWN: break;
            default: continue;
            }
            bool valid = true;
            for (std::uint32_t i = 0; i < info.element_count; ++i)
            {
                const auto& child = info.elements[i];
                if (!std::memchr(child.name, 0, sizeof(child.name)) ||
                    !std::memchr(child.executable, 0, sizeof(child.executable)) || child.process_alive > 1 ||
                    child.pid < -1 || child.pid == 0 || (child.pid == -1 && (child.process_start_ticks || child.process_alive))) valid = false;
            }
            if (!valid || !detail::ReadProcessIdentity(pid, actual) || actual != expected) continue;
            found.push_back(info);
        }
        supervisors.swap(found);
        return 0;
    }
    catch (const std::bad_alloc&) { return -ENOMEM; }
    catch (...) { return -EIO; }
}
}

namespace kcf
{
int IntrospectionClient::ListTypes(const RuntimeInfo& runtime, std::vector<TypeDescriptor>& types)
{
    try {
        std::uint64_t ticks=0;
        if(runtime.pid<=0 || !runtime.process_start_ticks) return -EINVAL;
        if(!detail::ReadProcessIdentity(runtime.pid,ticks) || ticks!=runtime.process_start_ticks) return -ENOENT;
        SharedChannel<detail::TypeRegistrySnapshot> storage;
        int result=storage.Open(detail::TypeRegistryName(runtime.pid,ticks));
        if(result) return result;
        auto snapshot=std::make_unique<detail::TypeRegistrySnapshot>();std::uint32_t sequence=0;
        result=storage.ReadLatestSnapshot(*snapshot,sequence);(void)storage.Close();
        if(result) return result;
        if(snapshot->protocol_version!=1 || snapshot->struct_size!=sizeof(*snapshot) || snapshot->pid!=runtime.pid ||
           snapshot->process_start_ticks!=runtime.process_start_ticks || snapshot->type_count>detail::MAX_TYPES) return -EPROTO;
        std::vector<TypeDescriptor> found;found.reserve(snapshot->type_count);
        for(std::uint32_t i=0;i<snapshot->type_count;++i) {
            const auto& type=snapshot->types[i];
            if(!ValidateTypeDescriptor(type)) return -EPROTO;
            for(const auto& previous:found) if(previous.type_id==type.type_id) return -EPROTO;
            found.push_back(type);
        }
        if(!detail::ReadProcessIdentity(runtime.pid,ticks) || ticks!=runtime.process_start_ticks) return -ENOENT;
        types.swap(found);return 0;
    } catch(const std::bad_alloc&) { return -ENOMEM; }
      catch(...) { return -EIO; }
}
int IntrospectionClient::GetType(const RuntimeInfo& runtime, std::uint64_t type_id, TypeDescriptor& descriptor)
{
    if(!type_id) return -EINVAL;
    std::vector<TypeDescriptor> types;
    const int result=ListTypes(runtime,types);
    if(result) return result;
    for(const auto& type:types) if(type.type_id==type_id) { descriptor=type;return 0; }
    return -ENOENT;
}
}

#include "kcf/introspection/detail/service_registry.hpp"
#include "kcf/service/service_protocol.hpp"
namespace kcf {
int IntrospectionClient::ListServices(const RuntimeInfo& runtime,std::vector<ServiceInfo>& services) {
    try{
        std::uint64_t ticks=0;
        if(runtime.pid<=0||!runtime.process_start_ticks)return -EINVAL;
        if(!detail::ReadProcessIdentity(runtime.pid,ticks)||ticks!=runtime.process_start_ticks)return -ENOENT;
        SharedChannel<detail::ServiceRegistrySnapshot> storage;
        int result=storage.Open(detail::ServiceRegistryName(runtime.pid,ticks));if(result)return result;
        auto snapshot=std::make_unique<detail::ServiceRegistrySnapshot>();std::uint32_t sequence=0;
        result=storage.ReadLatestSnapshot(*snapshot,sequence);(void)storage.Close();if(result)return result;
        if(snapshot->protocol_version!=1||snapshot->struct_size!=sizeof(*snapshot)||snapshot->pid!=runtime.pid||
           snapshot->process_start_ticks!=ticks||snapshot->service_count>detail::MAX_SERVICES)return -EPROTO;
        std::vector<ServiceInfo> found;std::vector<TypeDescriptor> types;bool loaded=false;
        for(std::uint32_t i=0;i<snapshot->service_count;++i){const auto& e=snapshot->services[i];
            if(!e.registration_id||!e.port||!e.request_size||!e.response_size||e.request_size>SERVICE_MAX_PAYLOAD||e.response_size>SERVICE_MAX_PAYLOAD||
               !e.name[0]||!std::memchr(e.name,0,sizeof(e.name))||
               !std::memchr(e.diagnostic_request_type_name,0,sizeof(e.diagnostic_request_type_name))||
               !std::memchr(e.diagnostic_response_type_name,0,sizeof(e.diagnostic_response_type_name)))return -EPROTO;
            for(const auto& old:found)if(old.registration_id==e.registration_id||(old.port==e.port&&old.service_id==e.service_id))return -EPROTO;
            if(e.request_type_id||e.response_type_id){
                if(!loaded){result=ListTypes(runtime,types);if(result)return result;loaded=true;}
                const auto matches=[&](std::uint64_t id,std::uint32_t size){if(!id)return true;for(const auto& d:types)if(d.type_id==id)return d.payload_size==size;return false;};
                if(!matches(e.request_type_id,e.request_size)||!matches(e.response_type_id,e.response_size))return -EPROTO;
            }
            found.push_back(e);
        }
        if(!detail::ReadProcessIdentity(runtime.pid,ticks)||ticks!=runtime.process_start_ticks)return -ENOENT;
        services.swap(found);return 0;
    }catch(const std::bad_alloc&){return -ENOMEM;}catch(...){return -EIO;}
}
}
