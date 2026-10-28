#pragma once
#include "kcf/network/parameter_codec.hpp"
#include "kcf/introspection/action_info.hpp"
namespace kcf::network {
// Local configuration only; no wire/SHM layout change. Prefixes are absolute
// paths, optional trailing '/', no empty/dot segments, whitespace or glob syntax.
// '/' explicitly matches all. '/robot' and '/robot/' both match /robot and its
// descendants, never /robot2. At most 64 rules in each list.
struct TopicExposurePolicy {
    bool enabled{false};
    std::vector<std::string> allow_prefixes;
    std::vector<std::string> exclude_prefixes;
};
// Validates even disabled policies. Canonical duplicates within a list return
// -EEXIST; overlap across allow/exclude is legal (exclude wins). -EINVAL malformed,
// -E2BIG rule limit. No registration, network, or other side effects.
int ValidateTopicExposurePolicy(const TopicExposurePolicy&);
struct MetadataConfig {
    std::vector<std::string> remote_topics; // exact Publisher/Subscriber names, default LOCAL
    std::vector<std::string> remote_actions; // existing status topic names, default LOCAL
    HostIdentity host,boot_id;
    std::vector<std::string> remote_services; // exact names, default LOCAL
    std::vector<std::string> remote_parameters; // exact owner names, default none
    std::vector<std::string> groups;
    std::vector<std::int32_t> runtime_pids; // optional namespace/test partition; empty = all live local runtimes
    TopicExposurePolicy topic_exposure; // appended to preserve existing aggregate initialization
};
// Pure Topic-only evaluation. Existing remote_topics enables exact Publisher OR
// Subscriber names. Enabled policy adds Publishers only; exclusions veto even
// an explicitly listed Publisher. Policy never modifies explicit Subscriber grants.
// Output unchanged on error. Not a permission grant for Control or Topic consumers.
int EvaluateTopicExposure(const MetadataConfig&, const std::string& topic_name,
                          EndpointRole, NetworkScope& output);
struct LocalEndpoint {
    NetworkEndpointMetadata metadata;
    TypeDescriptor descriptor; // Parameter/Topic value or Service request
    TypeDescriptor response_descriptor,feedback_descriptor;
    ActionInfo action;
    std::uint64_t service_registration{0};
    std::uint16_t service_port{0}, service_id{0};
};
// Caller runs on a non-runtime worker. No system-wide atomic snapshot.
// Revalidate registration and named storage owner immediately before access.
int ValidateLocalTopicEndpoint(const LocalEndpoint&,bool publisher);
int ValidateLocalActionEndpoint(const LocalEndpoint&);
int ValidateLocalServiceEndpoint(const LocalEndpoint&);
int ValidateLocalParameterEndpoint(const LocalEndpoint&);
int CollectLocalMetadata(const MetadataConfig&,std::vector<LocalEndpoint>&);
}
