#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include <algorithm>
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
        std::lock_guard<std::mutex> lock(mutex);
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
        if (!Active() || snapshot.endpoint_count >= MAX_ENDPOINTS ||
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
void UnregisterEndpoint(std::uint64_t id) noexcept
{
    if (!id) return;
    try
    {
        std::lock_guard<std::mutex> lock(mutex);
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
