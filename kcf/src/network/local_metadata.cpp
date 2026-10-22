#include "kcf/network/local_metadata.hpp"
#include "kcf/network/discovery_protocol.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include <algorithm>
#include <cerrno>
#include "kcf/ipc/detail/storage_layout.hpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
namespace kcf::network {
int ValidateLocalTopicEndpoint(const LocalEndpoint& entry,bool publisher) {
    RuntimeInfo runtime;runtime.pid=entry.metadata.identity.runtime.pid;runtime.process_start_ticks=entry.metadata.identity.runtime.process_start_ticks;
    IntrospectionClient client;std::vector<EndpointInfo> endpoints;int code=client.ListEndpoints(runtime,endpoints);if(code)return code;
    bool found=false;
    for(const auto& e:endpoints)if(e.registration_id==entry.metadata.identity.registration_id&&e.kind==kcf::EndpointKind::TOPIC&&
        e.role==(publisher?kcf::EndpointRole::PUBLISHER:kcf::EndpointRole::SUBSCRIBER)&&e.name==entry.metadata.name&&e.type_id==entry.descriptor.type_id&&e.payload_size==entry.descriptor.payload_size){found=true;break;}
    if(!found)return -ESTALE;
    if(!publisher)return 0;
    std::string encoded="/";for(std::size_t i=1;i<entry.metadata.name.size();++i){auto c=entry.metadata.name[i];if(c=='/')encoded+="%2F";else if(c=='%')encoded+="%25";else encoded+=c;}
    int fd=shm_open(encoded.c_str(),O_RDONLY|O_CLOEXEC,0);if(fd<0)return -errno;
    kcf::detail::ChannelHeader header{};auto count=pread(fd,&header,offsetof(kcf::detail::ChannelHeader,publish_sequence),0);close(fd);
    if(count!=offsetof(kcf::detail::ChannelHeader,publish_sequence)||header.owner_pid!=runtime.pid||header.magic!=kcf::detail::TOPIC_MAGIC||header.format!=kcf::detail::TOPIC_STORAGE_FORMAT||header.initialized!=1)return -ESTALE;
    return 0;
}
int ValidateLocalParameterEndpoint(const LocalEndpoint& entry) {
    const auto& m=entry.metadata;RuntimeInfo runtime;
    runtime.pid=m.identity.runtime.pid;runtime.process_start_ticks=m.identity.runtime.process_start_ticks;
    IntrospectionClient client;std::vector<EndpointInfo> endpoints;
    int code=client.ListEndpoints(runtime,endpoints);if(code)return code;
    if(std::none_of(endpoints.begin(),endpoints.end(),[&](const auto& e){return e.registration_id==m.identity.registration_id &&
        e.kind==kcf::EndpointKind::PARAMETER && e.role==kcf::EndpointRole::PARAMETER_OWNER &&
        e.name==m.name && e.type_id==entry.descriptor.type_id;}))return -ESTALE;
    std::string encoded="/";
    for(std::size_t i=1;i<m.name.size();++i) {
        if(m.name[i]=='/')encoded+="%2F";else if(m.name[i]=='%')encoded+="%25";else encoded+=m.name[i];
    }
    int fd=shm_open(encoded.c_str(),O_RDONLY|O_CLOEXEC,0);if(fd<0)return -errno;
    // Only inspect the existing Local prefix, never transmit it or change it.
    kcf::detail::ParameterStorage<std::byte> header{};
    const auto length=offsetof(kcf::detail::ParameterStorage<std::byte>,mutex);
    const auto count=pread(fd,&header,length,0);const int saved=errno;close(fd);
    if(count<0)return -saved;
    if(count!=static_cast<ssize_t>(length)||header.magic!=kcf::detail::PARAMETER_MAGIC||
        header.format!=kcf::detail::STORAGE_FORMAT||header.initialized!=1||header.owner_pid!=runtime.pid)return -ESTALE;
    return 0;
}
int ValidateLocalServiceEndpoint(const LocalEndpoint& entry) {
    RuntimeInfo runtime;runtime.pid=entry.metadata.identity.runtime.pid;
    runtime.process_start_ticks=entry.metadata.identity.runtime.process_start_ticks;
    IntrospectionClient client;std::vector<ServiceInfo> services;
    const int code=client.ListServices(runtime,services);if(code)return code;
    for(const auto& service:services)if(service.registration_id==entry.service_registration &&
        service.port==entry.service_port && service.service_id==entry.service_id && service.name==entry.metadata.name &&
        service.request_type_id==entry.descriptor.type_id && service.response_type_id==entry.response_descriptor.type_id &&
        service.request_size==entry.descriptor.payload_size && service.response_size==entry.response_descriptor.payload_size)return 0;
    return -ESTALE;
}
int ValidateLocalActionEndpoint(const LocalEndpoint& entry) {
    RuntimeInfo runtime;runtime.pid=entry.metadata.identity.runtime.pid;
    runtime.process_start_ticks=entry.metadata.identity.runtime.process_start_ticks;
    IntrospectionClient client;std::vector<ActionInfo> actions;int code=client.ListActions(runtime,actions);if(code)return code;
    for(const auto& action:actions)if(action.registration_id==entry.action.registration_id &&
        action.port==entry.action.port && action.ids.goal_service_id==entry.action.ids.goal_service_id &&
        action.ids.cancel_service_id==entry.action.ids.cancel_service_id && action.ids.result_service_id==entry.action.ids.result_service_id &&
        action.name==entry.metadata.name && action.goal_type_id==entry.descriptor.type_id &&
        action.feedback_type_id==entry.feedback_descriptor.type_id && action.result_type_id==entry.response_descriptor.type_id)return 0;
    return -ESTALE;
}
int CollectLocalMetadata(const MetadataConfig& config,std::vector<LocalEndpoint>& output) {
    if(!Valid(config.host)||!Valid(config.boot_id))return -EINVAL;
    IntrospectionClient client;std::vector<RuntimeInfo> runtimes;std::vector<SupervisorInfo> supervisors;
    int result=client.ListRuntimes(runtimes);if(result)return result;
    result=client.ListSupervisors(supervisors);if(result)return result;
    std::sort(runtimes.begin(),runtimes.end(),[](const auto& a,const auto& b){return a.pid<b.pid;});
    std::vector<LocalEndpoint> found;
    for(const auto& runtime:runtimes) {
        if(runtime.state!=ProcessState::RUNNING && runtime.state!=ProcessState::STARTING)continue;
        if(!config.runtime_pids.empty() && std::find(config.runtime_pids.begin(),config.runtime_pids.end(),runtime.pid)==config.runtime_pids.end())continue;
        std::vector<EndpointInfo> endpoints;(void)client.ListEndpoints(runtime,endpoints);
        std::sort(endpoints.begin(),endpoints.end(),[](const auto& a,const auto& b){return a.registration_id<b.registration_id;});
        for(const auto& endpoint:endpoints) {
            LocalEndpoint entry;auto& m=entry.metadata;
            if(endpoint.registration_id >= (1ull<<62))return -EOVERFLOW;
            if(!endpoint.type_id || client.GetType(runtime,endpoint.type_id,entry.descriptor) || ParameterWireType(entry.descriptor,m.value_type))continue;
            if(entry.descriptor.payload_size!=endpoint.payload_size)continue;
            m.identity={{config.host,config.boot_id,runtime.pid,runtime.process_start_ticks},endpoint.registration_id};
            m.kind=endpoint.kind==kcf::EndpointKind::PARAMETER?EndpointKind::PARAMETER:EndpointKind::TOPIC;
            m.role=(endpoint.role==kcf::EndpointRole::PARAMETER_OWNER || endpoint.role==kcf::EndpointRole::PUBLISHER)?EndpointRole::PROVIDER:EndpointRole::CONSUMER;
            m.name=endpoint.name;m.element_name=runtime.executable;m.groups=config.groups;
            for(const auto& s:supervisors)for(std::uint32_t i=0;i<s.element_count;++i) {
                const auto& e=s.elements[i];if(e.pid!=runtime.pid || e.process_start_ticks!=runtime.process_start_ticks)continue;
                m.application={config.host,config.boot_id,s.pid,s.process_start_ticks};m.application_name=s.application_name;m.element_name=e.name;
            }
            if(m.kind==EndpointKind::PARAMETER && m.role==EndpointRole::PROVIDER &&
                std::find(config.remote_parameters.begin(),config.remote_parameters.end(),m.name)!=config.remote_parameters.end())m.scope=NetworkScope::REMOTE;
            if(m.kind==EndpointKind::TOPIC&&std::find(config.remote_topics.begin(),config.remote_topics.end(),m.name)!=config.remote_topics.end())m.scope=NetworkScope::REMOTE;
            if(!Valid(m))return -EINVAL;
            found.push_back(std::move(entry));if(found.size()>DISCOVERY_MAX_ENDPOINTS)return -E2BIG;
        }
        std::vector<ServiceInfo> services;(void)client.ListServices(runtime,services);
        std::sort(services.begin(),services.end(),[](const auto& a,const auto& b){return a.registration_id<b.registration_id;});
        for(const auto& service:services) {
            if(service.registration_id >= (1ull<<63))return -EOVERFLOW;
            LocalEndpoint entry;auto& m=entry.metadata;
            if(!service.request_type_id || !service.response_type_id ||
               client.GetType(runtime,service.request_type_id,entry.descriptor) ||
               client.GetType(runtime,service.response_type_id,entry.response_descriptor) ||
               ParameterWireType(entry.descriptor,m.request_type) || ParameterWireType(entry.response_descriptor,m.response_type))continue;
            if(entry.descriptor.payload_size!=service.request_size || entry.response_descriptor.payload_size!=service.response_size)continue;
            m.identity={{config.host,config.boot_id,runtime.pid,runtime.process_start_ticks},service.registration_id|(1ull<<63)};
            m.kind=EndpointKind::SERVICE;m.role=EndpointRole::PROVIDER;m.name=service.name;m.element_name=runtime.executable;m.groups=config.groups;
            for(const auto& s:supervisors)for(std::uint32_t i=0;i<s.element_count;++i) {
                const auto& e=s.elements[i];if(e.pid!=runtime.pid || e.process_start_ticks!=runtime.process_start_ticks)continue;
                m.application={config.host,config.boot_id,s.pid,s.process_start_ticks};m.application_name=s.application_name;m.element_name=e.name;
            }
            if(std::find(config.remote_services.begin(),config.remote_services.end(),m.name)!=config.remote_services.end())m.scope=NetworkScope::REMOTE;
            entry.service_registration=service.registration_id;entry.service_port=service.port;entry.service_id=service.service_id;
            if(!Valid(m))return -EINVAL;
            found.push_back(std::move(entry));if(found.size()>DISCOVERY_MAX_ENDPOINTS)return -E2BIG;
        }
        std::vector<ActionInfo> actions;(void)client.ListActions(runtime,actions);
        std::sort(actions.begin(),actions.end(),[](const auto& a,const auto& b){return a.registration_id<b.registration_id;});
        for(const auto& action:actions) {
            if(action.registration_id >= (1ull<<62))return -EOVERFLOW;
            LocalEndpoint entry;auto& m=entry.metadata;
            if(client.GetType(runtime,action.goal_type_id,entry.descriptor)||client.GetType(runtime,action.feedback_type_id,entry.feedback_descriptor)||
               client.GetType(runtime,action.result_type_id,entry.response_descriptor)||ParameterWireType(entry.descriptor,m.request_type)||
               ParameterWireType(entry.feedback_descriptor,m.feedback_type)||ParameterWireType(entry.response_descriptor,m.response_type))continue;
            if(std::uint64_t(action.goal_offset)+entry.descriptor.payload_size>action.goal_request_size||
               std::uint64_t(action.feedback_offset)+entry.feedback_descriptor.payload_size>action.status_size||
               std::uint64_t(action.result_offset)+entry.response_descriptor.payload_size>action.result_response_size)continue;
            m.identity={{config.host,config.boot_id,runtime.pid,runtime.process_start_ticks},action.registration_id|(1ull<<62)};
            m.kind=EndpointKind::ACTION;m.role=EndpointRole::PROVIDER;m.name=action.name;m.element_name=runtime.executable;m.groups=config.groups;
            for(const auto& s:supervisors)for(std::uint32_t i=0;i<s.element_count;++i){const auto& e=s.elements[i];
                if(e.pid!=runtime.pid||e.process_start_ticks!=runtime.process_start_ticks)continue;
                m.application={config.host,config.boot_id,s.pid,s.process_start_ticks};m.application_name=s.application_name;m.element_name=e.name;
            }
            if(std::find(config.remote_actions.begin(),config.remote_actions.end(),m.name)!=config.remote_actions.end())m.scope=NetworkScope::REMOTE;
            entry.action=action;if(!Valid(m))return -EINVAL;
            found.push_back(std::move(entry));if(found.size()>DISCOVERY_MAX_ENDPOINTS)return -E2BIG;
        }
    }
    output=std::move(found);return 0;
}
}
