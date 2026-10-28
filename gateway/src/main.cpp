#include "gateway_support.hpp"
#include "local_ipc.hpp"
#include "network_lifecycle.hpp"
#include "kcf/network/host_identity.hpp"
#include <csignal>
#include <iostream>
#include <thread>
namespace n=kcf::network;
namespace {
volatile std::sig_atomic_t stopping=0;
void Signal(int signal){stopping=signal;}
std::string Hex(const n::HostIdentity& id){std::string out;for(auto b:id.bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;}
int Error(const char* stage,int code){std::cerr<<"[Gateway] "<<stage<<" error="<<code<<'\n';return 1;}
}
int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if(argc==2&&std::string(argv[1])=="--help") {
        std::cout<<"kcf_gateway [--state-dir DIR] [--lock-file PATH] [--ipc-socket PATH] [--interface NAME] [--discovery-port N] [--control-port N] [--topic-port N] [--allow-loopback] [--diagnostics] [--topic-allow-prefix PREFIX] [--topic-exclude-prefix PREFIX] [--topic-rate TOPIC:HZ] [--topic-fresh-start TOPIC] [--remote-topic TOPIC]\n"
                 <<"State directory must already exist; default /var/lib/kcf. Test loopback requires explicit interface.\n"
                 <<"Account-owned runtime lock: /run/kcf/gateway.lock; parent must exist and be protected. Never unlinked.\n"
                 <<"Metadata ON; repeatable Topic prefixes opt in Publisher exposure/data plane. Public Topic VALUE schema access follows allow prefixes; Control grants default OFF; Diagnostics OFF unless --diagnostics. Trusted LAN only. Network recovery enabled; no Control request replay.\n";
        std::cout<<"Fresh-start waits for a new Local Publish on every new route; it grants no exposure. Topic rates (0.1..1000 Hz) replace the default 50 Hz per exact name; they grant no access.\n"
                 <<"--remote-topic opts into the legacy exact-name REMOTE list (both roles), including reception; requires a local consumer descriptor/bootstrap. Publisher prefixes alone never opt in reception.\n";
        std::cout<<"--remote-parameter NAME / --remote-service NAME / --remote-action NAME: repeatable exact-name exposure, default empty.\n"
                 <<"--descriptor-client HOST_ID / --parameter-get-client HOST_ID / --parameter-set-client HOST_ID\n"
                 <<"--service-call-client HOST_ID / --action-goal-client HOST_ID / --action-cancel-client HOST_ID\n"
                 <<"Each client option independently grants that permission; repeatable, at most 32 unique nonzero 32-hex-digit Host IDs per option (no hyphens). Default empty. Exposure alone grants no client permission.\n";
        return 0;
    }
    try {
        gateway::Options options;
        try{options=gateway::Parse(argc,argv);}catch(const std::exception& e){std::cerr<<"Arguments: "<<e.what()<<'\n';return 2;}
        struct sigaction action{};action.sa_handler=Signal;sigemptyset(&action.sa_mask);
        if(sigaction(SIGINT,&action,nullptr)||sigaction(SIGTERM,&action,nullptr))return Error("signal",-errno);
        gateway::Fd lock;int code=gateway::Lock(lock,options.lock_file);if(code)return Error("singleton lock",code);
        n::HostIdentity host,boot;
        code=n::LoadOrCreateHostIdentity(options.state_dir+"/host-id",host);if(code)return Error("persistent Host ID",code);
        try{gateway::ValidateClientHosts(options,host);}catch(const std::exception& e){std::cerr<<"Arguments: "<<e.what()<<'\n';return 2;}
        code=n::ReadLinuxBootIdentity(boot);if(code)return Error("boot ID",code);
        gateway::local::Server ipc;
        gateway::local::Status local_status;local_status.host=host;local_status.boot=boot;
        ipc.Publish(local_status,{});
        code=ipc.Start(options.ipc_socket);if(code)return Error("local IPC",code);
        std::cout<<"[Gateway] local_ipc="<<options.ipc_socket<<" uid="<<geteuid()<<'\n';
        std::unique_ptr<n::ControlNode> node;
        std::cout<<"[Gateway] host="<<Hex(host)<<" boot="<<Hex(boot)<<" pid="<<getpid()<<'\n';
        gateway::NetworkLifecycle lifecycle({
            [&](n::NetworkInterfaceSelection& nic){return n::ResolveNetworkInterface(options.nic,nic);},
            [&](const n::NetworkInterfaceSelection& nic){
                local_status.nic=nic;
                auto config=gateway::Config(options,nic);config.discovery.host=host;config.discovery.boot_id=boot;
                int result=gateway::Preflight(config);if(result){Error("port preflight",result);return result;}
                if(stopping)return -ECANCELED;
                std::cout<<"[Gateway] starting interface="<<nic.name<<" ip="<<nic.ipv4_address
                         <<" index="<<nic.index<<" automatic="<<nic.automatic
                         <<" discovery_udp="<<options.discovery<<" control_tcp/metadata_udp="<<options.control
                         <<" topic_udp="<<options.topic<<" metadata=ON public_topic_descriptor="<<(config.topic_observer_enabled?"PUBLIC_TOPIC_VALUE":"NONE")<<" diagnostics="<<(config.diagnostics_enabled?"ON":"OFF")<<" topic="<<(config.topics.enabled?"ON":"OFF")
                         <<" topic_max_hz="<<config.topics.max_hz<<" allow_prefixes="<<options.topic_policy.allow_prefixes.size()
                         <<" exclude_prefixes="<<options.topic_policy.exclude_prefixes.size()<<'\n';
                for(const auto& name:config.topics.fresh_start_topics)std::cout<<"[Gateway] topic_fresh_start="<<name<<'\n';
                for(const auto& rate:config.topics.rate_limits)std::cout<<"[Gateway] topic_rate="<<rate.name<<":"<<rate.max_hz<<'\n';
                for(const auto& name:config.metadata.remote_topics)std::cout<<"[Gateway] legacy_remote_topic="<<name<<'\n';
                for(const auto& name:config.metadata.remote_parameters)std::cout<<"[Gateway] remote_parameter="<<name<<'\n';
                for(const auto& name:config.metadata.remote_services)std::cout<<"[Gateway] remote_service="<<name<<'\n';
                for(const auto& name:config.metadata.remote_actions)std::cout<<"[Gateway] remote_action="<<name<<'\n';
                std::cout<<"[Gateway] descriptor_clients="<<config.descriptor_query_clients.size()
                         <<" parameter_get_clients="<<config.parameter_get_clients.size()
                         <<" parameter_set_clients="<<config.parameter_set_clients.size()
                         <<" service_call_clients="<<config.service_call_clients.size()
                         <<" action_goal_clients="<<config.action_goal_clients.size()
                         <<" action_cancel_clients="<<config.action_cancel_clients.size()<<'\n';
                node=std::make_unique<n::ControlNode>();result=node->Start(config);
                if(result){node->Stop();node.reset();return result;}
                std::cout<<"[Gateway] session="<<node->GetSessionId()<<'\n';
                std::cout<<"[Gateway] ready\n";return 0;
            },
            [&]{local_status.state=gateway::NetworkState::REBINDING;local_status.session=0;local_status.nic={};ipc.Publish(local_status,{});if(node){node->Stop();node.reset();}std::cout<<"[Gateway] generation closed session=0\n";},
            [&]{return node?node->GetLastError():0;},
            [&]{return stopping!=0;},
            [&](gateway::NetworkState state,const char* reason,int error){
                local_status.state=state;if(error)local_status.error=error;
                local_status.session=node?node->GetSessionId():0;
                ipc.Publish(local_status,node?node->GetPeers():std::vector<n::PeerInfo>{});
                std::cout<<"[Gateway] state="<<gateway::StateName(state)<<" reason="<<reason<<" error="<<error
                         <<" recovery_attempts="<<lifecycle.Attempts()<<" successful_restarts="<<lifecycle.Restarts()
                         <<" session="<<local_status.session<<" last_recovery_error="<<lifecycle.LastRecoveryError()<<'\n';
            }
        });
        while(!stopping){
            lifecycle.Step(std::chrono::steady_clock::now());
            local_status.state=lifecycle.State();local_status.error=lifecycle.LastRecoveryError();
            local_status.session=node?node->GetSessionId():0;
            ipc.Publish(local_status,node?node->GetPeers():std::vector<n::PeerInfo>{});
            if(ipc.GetLastError()){Error("local IPC worker",ipc.GetLastError());break;}
            if(lifecycle.State()==gateway::NetworkState::ERROR)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        lifecycle.Stop();
        code=stopping?0:(ipc.GetLastError()?ipc.GetLastError():lifecycle.LastError());
        ipc.Stop();
        std::cout<<"[Gateway] stopped reason="<<(stopping?"signal":"network-error")<<" signal="<<stopping
                 <<" error="<<code<<" recovery_attempts="<<lifecycle.Attempts()<<" successful_restarts="<<lifecycle.Restarts()<<'\n';
        return code?1:0;
    }catch(const std::exception& e){std::cerr<<"[Gateway] exception: "<<e.what()<<'\n';return 1;}
}
