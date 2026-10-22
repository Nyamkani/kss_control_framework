#pragma once
#include "kcf/network/parameter_codec.hpp"
#include "kcf/introspection/action_info.hpp"
namespace kcf::network {
struct MetadataConfig {
    std::vector<std::string> remote_topics; // exact Publisher/Subscriber names, default LOCAL
    std::vector<std::string> remote_actions; // existing status topic names, default LOCAL
    HostIdentity host,boot_id;
    std::vector<std::string> remote_services; // exact names, default LOCAL
    std::vector<std::string> remote_parameters; // exact owner names, default none
    std::vector<std::string> groups;
    std::vector<std::int32_t> runtime_pids; // optional namespace/test partition; empty = all live local runtimes
};
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
