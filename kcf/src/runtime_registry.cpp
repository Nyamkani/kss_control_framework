#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/detail/type_registry.hpp"
#include "kcf/introspection/detail/service_registry.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <cstring>
#include <unistd.h>

namespace kcf::detail
{
namespace
{
template<class T> bool Number(std::string_view text, T& value)
{
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0;
}
// ProcessRuntime already requires one active runtime per process.
std::mutex registry_mutex;
std::unique_ptr<SharedParameter<RuntimeInfo>> registry;
RuntimeInfo current;
void Update(ProcessState state, int error)
{
    if (!registry) return;
    current.state = state;
    current.runtime_error = error;
    (void)registry->Set(current);
}
}
bool ParseProcessStat(std::string_view text, std::uint64_t& ticks, char& state)
{
    const auto end = text.rfind(')');
    if (end == std::string_view::npos || text.find('(') == std::string_view::npos) return false;
    std::istringstream fields(std::string{text.substr(end + 1)});
    if (!(fields >> state)) return false; // field 3, after comm (including spaces/parentheses)
    std::string field;
    for (int index = 4; index <= 22; ++index)
    {
        if (!(fields >> field)) return false;
    }
    return Number(std::string_view(field), ticks);
}
bool ReadProcessIdentity(std::int32_t pid, std::uint64_t& ticks)
{
    if (pid <= 0) return false;
    std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
    // comm may contain newlines as well as spaces; read the entire stat record.
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    char state = 0;
    return ParseProcessStat(text, ticks, state) && state != 'Z' && state != 'X' && state != 'x';
}
bool ParseDiscoveryName(std::string_view name, std::string_view prefix, std::int32_t& pid, std::uint64_t& ticks)
{
    const auto prefix_size = prefix.size();
    if (name.substr(0, prefix_size) != prefix) return false;
    name.remove_prefix(prefix_size);
    const auto split = name.find('_');
    if (split == std::string_view::npos) return false;
    const auto p = name.substr(0, split), t = name.substr(split + 1);
    return !p.empty() && !t.empty() && p.front() != '0' && t.front() != '0' &&
           Number(p, pid) && Number(t, ticks);
}
bool ParseRuntimeName(std::string_view name, std::int32_t& pid, std::uint64_t& ticks)
{
    return ParseDiscoveryName(name, RUNTIME_REGISTRY_PREFIX, pid, ticks);
}
std::string CurrentExecutableBasename()
{
    std::string path(256, '\0');
    for (;;)
    {
        const auto count = readlink("/proc/self/exe", path.data(), path.size());
        if (count < 0) return "supervisor";
        if (static_cast<std::size_t>(count) < path.size()) { path.resize(count); break; }
        path.resize(path.size() * 2);
    }
    return path.substr(path.find_last_of('/') + 1);
}
std::string RuntimeRegistryName(std::int32_t pid, std::uint64_t ticks)
{
    return std::string("/") + RUNTIME_REGISTRY_PREFIX + std::to_string(pid) + "_" + std::to_string(ticks);
}
void BeginRuntimeIntrospection(ExecutionMode mode) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (registry) return;
        RuntimeInfo info{};
        info.pid = static_cast<std::int32_t>(getpid());
        if (!ReadProcessIdentity(info.pid, info.process_start_ticks)) return;
        info.execution_mode = mode;
        info.state = ProcessState::STARTING;
        const auto basename = CurrentExecutableBasename();
        const auto count = std::min(basename.size(), sizeof(info.executable) - 1);
        std::memcpy(info.executable, basename.data(), count);
        auto storage = std::make_unique<SharedParameter<RuntimeInfo>>();
        if (storage->Create(RuntimeRegistryName(info.pid, info.process_start_ticks), info) != 0) return;
        current = info;
        registry = std::move(storage);
        BeginTypeRegistry(info.pid, info.process_start_ticks);
        BeginEndpointRegistry(info.pid, info.process_start_ticks);
        BeginServiceRegistry(info.pid, info.process_start_ticks);
    }
    catch (...) {} // Introspection never changes Runtime's result.
}
void UpdateRuntimeIntrospection(ProcessState state, int runtime_error) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        Update(state, runtime_error);
    }
    catch (...) {}
}
void EndRuntimeIntrospection(ProcessState state, int runtime_error) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        EndServiceRegistry();
        EndEndpointRegistry();
        EndTypeRegistry();
        if (!registry) return;
        Update(state, runtime_error);
        (void)registry->Unlink();
        (void)registry->Close();
        registry.reset();
    }
    catch (...) {}
}
}
