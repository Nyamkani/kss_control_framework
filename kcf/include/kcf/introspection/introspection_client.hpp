#pragma once
#include "kcf/introspection/runtime_info.hpp"
#include "kcf/introspection/endpoint_info.hpp"
#include "kcf/introspection/supervisor_info.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include "kcf/introspection/service_info.hpp"
#include <vector>

namespace kcf
{
class IntrospectionClient
{
public:
    // Linux local, observational only. No control, unlink or owner operations.
    // 0 replaces output (possibly empty); negative errno leaves output unchanged.
    // Candidates may disappear during enumeration. No atomic system-wide snapshot.
    // Uses blocking low-rate storage access: do not call on a GUI event thread.
    int ListServices(const RuntimeInfo& runtime, std::vector<ServiceInfo>& services);
    int ListSupervisors(std::vector<SupervisorInfo>& supervisors);
    int ListRuntimes(std::vector<RuntimeInfo>& runtimes);
    // Targeted lookup; negative errno preserves output. No payload read/write.
    int ListTypes(const RuntimeInfo& runtime, std::vector<TypeDescriptor>& types);
    int GetType(const RuntimeInfo& runtime, std::uint64_t type_id, TypeDescriptor& descriptor);
    int ListEndpoints(const RuntimeInfo& runtime, std::vector<EndpointInfo>& endpoints);
};
}
