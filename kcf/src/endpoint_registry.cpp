#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unistd.h>
namespace kcf::detail
{
namespace
{
std::mutex mutex;
std::unique_ptr<SharedParameter<EndpointRegistrySnapshot>> storage;
EndpointRegistrySnapshot snapshot;
// Never reset IDs between Run calls in the same process generation: surviving
// wrappers from an earlier Run must not unregister a new endpoint accidentally.
std::uint64_t last_id = 0;
bool Active() { return storage && snapshot.pid == getpid(); }
struct Recovery {
    std::uint64_t id{0};
    EndpointTypeRegistrationStatus status{};
    std::unique_ptr<TypeDescriptor> descriptor;
};
std::array<Recovery, MAX_ENDPOINTS> recoveries;
std::condition_variable changed;
std::thread worker;
bool stopping=false;
// Only EAGAIN is retried, at most once per 50 ms per endpoint, 64 attempts.
// No work is scheduled by Publish/Read or a Runtime control loop.
void Recover() {
    std::unique_lock<std::mutex> lock(mutex);
    while(!stopping) {
        changed.wait(lock, [] {
            return stopping || std::any_of(recoveries.begin(),recoveries.end(),
                [](const Recovery& r){return r.status.pending;});
        });
        if(changed.wait_for(lock,std::chrono::milliseconds(50),[]{return stopping;})) break;
        for(auto& r:recoveries) {
            if(!r.status.pending) continue;
            if(!Active()) { r.status.pending=false;r.descriptor.reset();continue; }
            auto begin=snapshot.endpoints, end=begin+snapshot.endpoint_count;
            auto endpoint=std::find_if(begin,end,[&](const EndpointInfo& e){return e.registration_id==r.id;});
            if(endpoint==end) { r={};continue; }
            r.status.latest=RegisterTypeDetailed(*r.descriptor);
            ++r.status.recovery_attempts;
            if(r.status.latest.type_id) {
                if(snapshot.revision==std::numeric_limits<std::uint64_t>::max()) {
                    r.status.latest={0,-EOVERFLOW,TypeRegistrationFailure::RESOURCE};
                } else {
                    endpoint->type_id=r.status.latest.type_id;
                    ++snapshot.revision;
                    const int error=storage->Set(snapshot);
                    if(error) {
                        // Same post-commit failure contract as RegisterEndpoint.
                        (void)storage->Close();(void)storage->Unlink();storage.reset();
                        r.status.latest={0,error,TypeRegistrationFailure::SNAPSHOT_PUBLISH};
                    } else r.status.recovered=true;
                }
                r.status.pending=false;
            } else r.status.pending=r.status.latest.Retryable() && r.status.recovery_attempts<64;
            if(!r.status.pending) r.descriptor.reset();
        }
    }
}
// Runtime teardown normally joins explicitly. Also avoid terminate for legacy
// programs that exit without EndRuntimeIntrospection (destruction order matters).
struct WorkerCleanup {
    ~WorkerCleanup() {
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;changed.notify_all();}
        if(worker.joinable()) worker.join();
    }
};

}
std::string EndpointRegistryName(std::int32_t pid, std::uint64_t ticks)
{
    return std::string("/") + ENDPOINT_REGISTRY_PREFIX + std::to_string(pid) + "_" + std::to_string(ticks);
}
void BeginEndpointRegistry(std::int32_t pid, std::uint64_t ticks) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (storage) return;
        stopping=false;
        auto candidate = std::make_unique<SharedParameter<EndpointRegistrySnapshot>>();
        snapshot = {};
        snapshot.pid = pid; snapshot.process_start_ticks = ticks;
        if (candidate->Create(EndpointRegistryName(pid, ticks), snapshot) == 0)
            storage = std::move(candidate);
    }
    catch (...) {}
}
void EndEndpointRegistry() noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping=true;
            changed.notify_all();
        }
        // Never join while holding the endpoint mutex. Worker never acquires
        // the Runtime registry lock; endpoint -> type is the only nested order.
        if(worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& r:recoveries) r={};
        if (!Active()) return;
        (void)storage->Close();
        (void)storage->Unlink();
        storage.reset();
    }
    catch (...) {}
}
std::uint64_t RegisterEndpoint(EndpointKind kind, EndpointRole role,
    const std::string& name, std::uint64_t payload_size, const char* type, std::uint64_t type_id) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !Active() || snapshot.endpoint_count >= MAX_ENDPOINTS ||
            last_id == std::numeric_limits<std::uint64_t>::max() ||
            snapshot.revision == std::numeric_limits<std::uint64_t>::max() ||
            name.empty() || name.size() >= sizeof(EndpointInfo::name) ||
            name.find('\0') != std::string::npos || !type) return 0;
        EndpointInfo info{};
        info.registration_id = ++last_id;
        info.kind = kind; info.role = role; info.payload_size = payload_size; info.type_id = type_id;
        std::memcpy(info.name, name.data(), name.size());
        std::memcpy(info.diagnostic_type_name, type, std::min(std::strlen(type), sizeof(info.diagnostic_type_name) - 1));
        snapshot.endpoints[snapshot.endpoint_count++] = info;
        ++snapshot.revision;
        if (storage->Set(snapshot) != 0)
        {
            // Set can report a post-commit notification error. Abandon this
            // observational generation rather than expose an unowned ghost ID.
            (void)storage->Close(); (void)storage->Unlink(); storage.reset();
            return 0;
        }
        return info.registration_id;
    }
    catch (...) { return 0; }
}
std::uint64_t RegisterEndpointWithType(EndpointKind kind, EndpointRole role,
    const std::string& name, std::uint64_t size, const char* diagnostic,
    const TypeDescriptor* descriptor) noexcept {
    if(!descriptor) return RegisterEndpoint(kind,role,name,size,diagnostic,0);
    const auto result=RegisterTypeDetailed(*descriptor);
    const auto id=RegisterEndpoint(kind,role,name,size,diagnostic,result.type_id);
    if(!id) return 0;
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if(stopping || !Active()) return id;
        const auto end=snapshot.endpoints+snapshot.endpoint_count;
        if(std::none_of(snapshot.endpoints,end,[&](const EndpointInfo& e){return e.registration_id==id;})) return id;
        for(auto& r:recoveries) if(!r.id) {
            r.id=id;r.status.initial=result;r.status.latest=result;
            if(result.Retryable()) {
                try {
                    r.descriptor=std::make_unique<TypeDescriptor>(*descriptor);
                    // Register cleanup after global registry initialization, so it
                    // runs before other translation units destroy their storage.
                    static WorkerCleanup cleanup;
                    if(!worker.joinable()) worker=std::thread(Recover);
                    r.status.pending=true;
                    changed.notify_all();
                } catch(...) { r.descriptor.reset();r.status.latest={0,-ENOMEM,TypeRegistrationFailure::RESOURCE}; }
            }
            break;
        }
    } catch(...) {}
    return id;
}
bool GetEndpointTypeRegistrationStatus(std::uint64_t id, EndpointTypeRegistrationStatus& out) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        for(const auto& r:recoveries) if(id && r.id==id) {out=r.status;return true;}
    } catch(...) {}
    return false;
}
void UnregisterEndpoint(std::uint64_t id) noexcept
{
    if (!id) return;
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& r:recoveries) if(r.id==id) r={};
        if (!Active()) return;
        for (std::uint32_t i = 0; i < snapshot.endpoint_count; ++i)
        {
            if (snapshot.endpoints[i].registration_id != id) continue;
            snapshot.endpoints[i] = snapshot.endpoints[--snapshot.endpoint_count];
            snapshot.endpoints[snapshot.endpoint_count] = {};
            if (snapshot.revision == std::numeric_limits<std::uint64_t>::max())
            { (void)storage->Close(); (void)storage->Unlink(); storage.reset(); return; }
            ++snapshot.revision;
            if (storage->Set(snapshot) != 0)
            { (void)storage->Close(); (void)storage->Unlink(); storage.reset(); }
            return;
        }
    }
    catch (...) {}
}
}
