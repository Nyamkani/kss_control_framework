#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const char* ApplicationName(kcf::ApplicationState state) {
    switch(state) {
    case kcf::ApplicationState::INITIALIZING:return "INITIALIZING";
    case kcf::ApplicationState::RUNNING:return "RUNNING";
    case kcf::ApplicationState::ERROR:return "ERROR";
    case kcf::ApplicationState::RESETTING:return "RESETTING";
    case kcf::ApplicationState::SHUTTING_DOWN:return "SHUTTING_DOWN";
    case kcf::ApplicationState::STOPPED:return "STOPPED";
    }
    return "UNKNOWN";
}
const char* FailureName(kcf::ElementFailureKind reason) {
    switch(reason) {
    case kcf::ElementFailureKind::NONE:return "NONE";
    case kcf::ElementFailureKind::PROCESS_EXIT:return "PROCESS_EXIT";
    case kcf::ElementFailureKind::RUNTIME_ERROR:return "RUNTIME_ERROR";
    case kcf::ElementFailureKind::STATUS_TIMEOUT:return "STATUS_TIMEOUT";
    case kcf::ElementFailureKind::HEARTBEAT_STALL:return "HEARTBEAT_STALL";
    }
    return "UNKNOWN";
}
void Usage() {
    std::cout<<"kcf status | kcf topic list | kcf network status [SOCKET] | kcf doctor\n"
             <<"Read-only phase-1 CLI; other topic commands and JSON schema are not yet implemented.\n";
}
std::uint32_t ReadBig(const unsigned char* p,unsigned bytes) {
    std::uint32_t result=0;
    while(bytes--)result=(result<<8)|*p++;
    return result;
}
int SendExact(int fd,const void* buffer,std::size_t size) {
    const auto* bytes=static_cast<const unsigned char*>(buffer);
    while(size) {
        const ssize_t sent=send(fd,bytes,size,MSG_NOSIGNAL);
        if(sent<0){if(errno==EINTR)continue;return -errno;}
        if(sent==0)return -EIO;
        bytes+=sent;size-=static_cast<std::size_t>(sent);
    }
    return 0;
}
int ReadExact(int fd,void* buffer,std::size_t size) {
    auto* bytes=static_cast<unsigned char*>(buffer);
    while(size) {
        const ssize_t result=recv(fd,bytes,size,0);
        if(result==0)return -ECONNRESET;
        if(result<0){if(errno==EINTR)continue;return -errno;}
        bytes+=result;size-=static_cast<std::size_t>(result);
    }
    return 0;
}
// This query uses the existing same-UID Gateway local-only IPC protocol v1.
// It does not grant Network Control permissions or expose a listener.
int NetworkStatus(const std::string& socket_path) {
    if(socket_path.empty()||socket_path.size()>=sizeof(sockaddr_un::sun_path))return -EINVAL;
    const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0)return -errno;
    timeval timeout{2,0};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::memcpy(address.sun_path,socket_path.c_str(),socket_path.size()+1);
    int result=0;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0)result=-errno;
    unsigned char request[16]{};
    // length u32=0, magic K C F L, version=1, op=GET_STATUS(1), status=0
    request[4]='K';request[5]='C';request[6]='F';request[7]='L';
    request[9]=1;request[11]=1;
    if(!result)result=SendExact(fd,request,sizeof(request));
    unsigned char header[16]{};
    if(!result)result=ReadExact(fd,header,sizeof(header));
    std::string body;
    if(!result) {
        const auto size=ReadBig(header,4);
        const auto magic=ReadBig(header+4,4);
        const auto version=ReadBig(header+8,2);
        const auto operation=ReadBig(header+10,2);
        const auto status=static_cast<std::int32_t>(ReadBig(header+12,4));
        if(magic!=0x4b43464c||version!=1||operation!=1||size>1024*1024)
            result=-EPROTO;
        else if(status)result=status;
        else {
            body.resize(size);
            if(size)result=ReadExact(fd,body.data(),body.size());
        }
    }
    close(fd);
    if(!result)std::cout<<body<<"\n";
    return result;
}
int Status() {
    kcf::IntrospectionClient client;
    std::vector<kcf::SupervisorInfo> supervisors;
    const int error=client.ListSupervisors(supervisors);
    if(error)return error;
    for(const auto& supervisor:supervisors) {
        std::cout<<"Application "<<supervisor.application_name
                 <<" pid="<<supervisor.pid<<" state="<<ApplicationName(supervisor.application_state)
                 <<" elements="<<supervisor.element_count<<"\n";
        kcf::SupervisorDiagnosticsInfo detail{};
        const int available=client.GetSupervisorDiagnostics(supervisor.pid,supervisor.process_start_ticks,detail);
        for(std::uint32_t i=0;i<supervisor.element_count;++i) {
            const auto& e=supervisor.elements[i];
            std::cout<<"  Element "<<e.name<<" pid="<<e.pid<<" alive="<<int(e.process_alive);
            if(!available && i<detail.element_count) {
                const auto& d=detail.elements[i];
                std::cout<<" failure="<<FailureName(d.failure_kind)
                         <<" detected="<<int(d.failure_detected)<<" runtime_error="<<d.runtime_error;
                if(d.exit_info_available)std::cout<<" exit="<<d.exit_code<<" signal="<<d.signal_number;
            }else std::cout<<" detailed_status=N/A";
            std::cout<<"\n";
        }
        if(!available) {
            std::cout<<"  History retained="<<detail.history_count
                     <<" generation="<<detail.generation
                     <<" latest_sequence="<<detail.last_error_sequence<<"\n";
            for(std::uint32_t i=0;i<detail.history_count;++i) {
                const auto& e=detail.history[i];
                std::cout<<"    #"<<e.sequence<<" gen="<<e.generation
                         <<" element="<<e.element_name<<" failure="<<FailureName(e.failure_kind)
                         <<" error="<<e.runtime_error<<"\n";
            }
        }else std::cout<<"  History N/A (diagnostics not published or unreadable: "<<available<<")\n";
    }
    std::vector<kcf::RuntimeInfo> runtimes;
    const int runtime_error=client.ListRuntimes(runtimes);
    if(runtime_error)return runtime_error;
    std::cout<<"Local runtimes="<<runtimes.size()<<", supervisors="<<supervisors.size()<<"\n";
    return 0;
}
int TopicList() {
    kcf::IntrospectionClient client;
    std::vector<kcf::RuntimeInfo> runtimes;
    const int code=client.ListRuntimes(runtimes);
    if(code)return code;
    unsigned count=0;
    for(const auto& runtime:runtimes) {
        std::vector<kcf::EndpointInfo> endpoints;
        if(client.ListEndpoints(runtime,endpoints)!=0)continue;
        for(const auto& endpoint:endpoints) {
            if(endpoint.kind!=kcf::EndpointKind::TOPIC)continue;
            std::cout<<endpoint.name<<" "<<(endpoint.role==kcf::EndpointRole::PUBLISHER?"PUB":"SUB")
                     <<" pid="<<runtime.pid<<" type="<<endpoint.diagnostic_type_name<<"\n";
            ++count;
        }
    }
    std::cout<<"Local Topic endpoints="<<count<<" (observed, not Network exposure)\n";
    return 0;
}
int Doctor() {
    kcf::IntrospectionClient client;
    std::vector<kcf::SupervisorInfo> supervisors;
    std::vector<kcf::RuntimeInfo> runtimes;
    int code=client.ListRuntimes(runtimes);
    if(code){std::cout<<"Runtime discovery: ERROR "<<code<<"\n";return code;}
    code=client.ListSupervisors(supervisors);
    if(code){std::cout<<"Supervisor discovery: ERROR "<<code<<"\n";return code;}
    std::cout<<"Runtime discovery: OK ("<<runtimes.size()<<")\n"
             <<"Supervisor discovery: OK ("<<supervisors.size()<<")\n"
             <<"Gateway / Remote Network diagnostics: N/A (not queried)\n"
             <<"No root cause inferred from absent endpoints.\n";
    return 0;
}
} // namespace
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="status")return Status()==0?0:1;
    if(argc==3 && std::string(argv[1])=="topic" && std::string(argv[2])=="list")
        return TopicList()==0?0:1;
    if((argc==3||argc==4)&&std::string(argv[1])=="network"&&std::string(argv[2])=="status") {
        const int code=NetworkStatus(argc==4?argv[3]:"/run/kcf/gateway.sock");
        if(code)std::cerr<<"Network status query error="<<code<<"\n";
        return code?1:0;
    }
    if(argc==2&&std::string(argv[1])=="doctor")return Doctor()==0?0:1;
    Usage();return argc==2 && (std::string(argv[1])=="--help"||std::string(argv[1])=="help")?0:2;
}
