#pragma once
#include "kcf/introspection/runtime_info.hpp"
#include <string>
#include <string_view>

namespace kcf::detail
{
inline constexpr char RUNTIME_REGISTRY_PREFIX[] = "kcf_introspection_runtime_";
// Internal helpers; not a public storage/protocol contract.
bool ParseProcessStat(std::string_view text, std::uint64_t& ticks, char& state);
bool ReadProcessIdentity(std::int32_t pid, std::uint64_t& ticks);
bool ParseDiscoveryName(std::string_view name, std::string_view prefix, std::int32_t& pid, std::uint64_t& ticks);
std::string CurrentExecutableBasename();
bool ParseRuntimeName(std::string_view name, std::int32_t& pid, std::uint64_t& ticks);
std::string RuntimeRegistryName(std::int32_t pid, std::uint64_t ticks);
void BeginRuntimeIntrospection(ExecutionMode mode) noexcept;
void UpdateRuntimeIntrospection(ProcessState state, int runtime_error) noexcept;
void EndRuntimeIntrospection(ProcessState state, int runtime_error) noexcept;
}
