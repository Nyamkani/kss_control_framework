#pragma once
#include "kcf/network/control_node.hpp"
#include "kcf/network/network_interface.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <algorithm>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gateway {
struct Fd {
    int value{-1};
    ~Fd(){if(value>=0)close(value);}
    Fd()=default;Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
struct Options {
    std::string state_dir{"/var/lib/kcf"};
    std::string ipc_socket{"/run/kcf/gateway.sock"};
    std::string lock_file{"/run/kcf/gateway.lock"};
    kcf::network::NetworkInterfaceOptions nic;
    kcf::network::TopicExposurePolicy topic_policy;
    std::vector<kcf::network::TopicRateLimit> topic_rates;
    std::vector<std::string> remote_topics;
    std::vector<std::string> fresh_start_topics;
    std::uint16_t discovery{37651},control{39001},topic{37652};
};
inline std::uint16_t Port(const std::string& text) {
    if(text.empty()||text.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("invalid port");
    auto n=std::stoul(text);if(n==0||n>65535)throw std::invalid_argument("port must be 1..65535");
    return static_cast<std::uint16_t>(n);
}
inline Options Parse(int argc,char** argv) {
    Options out;std::set<std::string> seen;
    for(int i=1;i<argc;++i){std::string key=argv[i];
        const bool prefix=key=="--topic-allow-prefix"||key=="--topic-exclude-prefix"||key=="--topic-rate"||key=="--remote-topic"||key=="--topic-fresh-start";
        if(!prefix&&!seen.insert(key).second)throw std::invalid_argument("duplicate option");
        if(key=="--allow-loopback"){out.nic.allow_loopback=true;continue;}
        if(!prefix&&key!="--ipc-socket"&&key!="--lock-file"&&key!="--state-dir"&&key!="--interface"&&key!="--discovery-port"&&key!="--control-port"&&key!="--topic-port")throw std::invalid_argument("unknown option");
        if(++i==argc||!*argv[i])throw std::invalid_argument("missing option value");
        std::string value=argv[i];
        if(key=="--topic-allow-prefix")out.topic_policy.allow_prefixes.push_back(value);
        if(key=="--topic-exclude-prefix")out.topic_policy.exclude_prefixes.push_back(value);
        if(key=="--topic-rate") {
            auto colon=value.rfind(':');
            if(colon==std::string::npos)throw std::invalid_argument("expected Topic:Hz");
            const auto name=value.substr(0,colon), number=value.substr(colon+1);
            std::size_t used=0;double hz=std::stod(number,&used);
            if(used!=number.size()||!std::isfinite(hz)||hz<0.1||hz>1000)throw std::invalid_argument("Topic Hz must be 0.1..1000");
            for(const auto& r:out.topic_rates)if(r.name==name)throw std::invalid_argument("duplicate Topic rate");
            out.topic_rates.push_back({name,hz});
        }
        if(key=="--topic-fresh-start") {
            if(std::find(out.fresh_start_topics.begin(),out.fresh_start_topics.end(),value)!=out.fresh_start_topics.end())throw std::invalid_argument("duplicate fresh-start Topic");
            out.fresh_start_topics.push_back(value);
        }
        if(key=="--remote-topic") {
            if(std::find(out.remote_topics.begin(),out.remote_topics.end(),value)!=out.remote_topics.end())throw std::invalid_argument("duplicate remote Topic");
            out.remote_topics.push_back(value);
        }
        if(key=="--state-dir")out.state_dir=value;
        if(key=="--ipc-socket")out.ipc_socket=value;
        if(key=="--lock-file")out.lock_file=value;
        if(key=="--interface")out.nic.interface_name=value;
        if(key=="--discovery-port")out.discovery=Port(value);
        if(key=="--control-port")out.control=Port(value);
        if(key=="--topic-port")out.topic=Port(value);
    }
    if(out.discovery==out.control||out.discovery==out.topic||out.control==out.topic)throw std::invalid_argument("UDP port numbers must differ");
    if(out.nic.allow_loopback && out.nic.interface_name.empty())throw std::invalid_argument("--allow-loopback requires --interface");
    auto valid_name=[](const std::string& name){return !name.empty()&&name.front()=='/'&&name.size()<=240&&name.find_first_of(" \t\r\n")==std::string::npos;};
    if(out.topic_rates.size()>64||out.remote_topics.size()>64||out.fresh_start_topics.size()>64)throw std::invalid_argument("too many Topic settings");
    for(const auto& r:out.topic_rates)if(!valid_name(r.name))throw std::invalid_argument("invalid Topic rate name");
    for(const auto& name:out.fresh_start_topics)if(!valid_name(name))throw std::invalid_argument("invalid fresh-start Topic name");
    for(const auto& name:out.remote_topics)if(!valid_name(name))throw std::invalid_argument("invalid remote Topic name");
    out.topic_policy.enabled=!out.topic_policy.allow_prefixes.empty()||!out.topic_policy.exclude_prefixes.empty();
    const int policy_error=kcf::network::ValidateTopicExposurePolicy(out.topic_policy);
    if(policy_error)throw std::invalid_argument("Topic policy error="+std::to_string(policy_error));
    return out;
}
inline kcf::network::ControlConfig Config(const Options& options,const kcf::network::NetworkInterfaceSelection& nic) {
    kcf::network::ControlConfig config;
    config.discovery.interface_address=nic.ipv4_address;
    config.discovery.port=options.discovery;config.discovery.metadata_port=options.control;
    config.topics.port=options.topic;
    config.metadata.topic_exposure=options.topic_policy;
    config.topics.enabled=options.topic_policy.enabled&&!options.topic_policy.allow_prefixes.empty();
    config.topic_observer_enabled=config.topics.enabled;
    config.metadata.remote_topics=options.remote_topics;
    config.topics.rate_limits=options.topic_rates;
    config.topics.fresh_start_topics=options.fresh_start_topics;
    config.topics.enabled=config.topics.enabled||!options.remote_topics.empty();
    return config; // Rate settings do not grant exposure or Control permissions.
}
// Shared deployment lock path, never unlink. A different override intentionally
// defines a different lock domain; all production launchers must use the same path.
inline int Lock(Fd& fd,const std::string& path="/run/kcf/gateway.lock") {
    if(path.empty()||path.front()!='/'||path.back()=='/'||path.find('\0')!=std::string::npos)return -EINVAL;
    Fd dir;dir.value=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);
    if(dir.value<0)return -errno;
    std::size_t start=1;
    while(true) {
        auto end=path.find('/',start);auto part=path.substr(start,end==std::string::npos?end:end-start);
        if(part.empty()||part=="."||part=="..")return -EINVAL;
        if(end==std::string::npos) {
            struct stat parent{};if(fstat(dir.value,&parent))return -errno;
            if(parent.st_uid!=geteuid()||(parent.st_mode&0022))return -EACCES;
            fd.value=openat(dir.value,part.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
            break;
        }
        int next=openat(dir.value,part.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC);
        if(next<0)return -errno;
        struct stat st{};
        if(fstat(next,&st)){int e=errno;close(next);return -e;}
        if(!S_ISDIR(st.st_mode)){close(next);return S_ISLNK(st.st_mode)?-ELOOP:-ENOTDIR;}
        close(dir.value);dir.value=next;start=end+1;
    }
    if(fd.value<0)return -errno;
    struct stat st{};if(fstat(fd.value,&st))return -errno;
    if(!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||(st.st_mode&0777)!=0600)return -EACCES;
    if(flock(fd.value,LOCK_EX|LOCK_NB))return errno==EWOULDBLOCK?-EBUSY:-errno;
    return 0;
}
inline int Probe(const std::string& ip,std::uint16_t port,int type,bool discovery=false) {
    Fd fd;fd.value=socket(AF_INET,type|SOCK_CLOEXEC,0);if(fd.value<0)return -errno;
    if(discovery || type==SOCK_STREAM){int one=1;if(setsockopt(fd.value,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)))return -errno;}
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1)return -EINVAL;
    if(bind(fd.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return -errno;
    return 0;
}
inline int Preflight(const kcf::network::ControlConfig& config) {
    int code=Probe("0.0.0.0",config.discovery.port,SOCK_DGRAM,true);if(code)return code;
    code=Probe(config.discovery.interface_address,config.discovery.metadata_port,SOCK_STREAM);if(code)return code;
    code=Probe(config.discovery.interface_address,config.discovery.metadata_port,SOCK_DGRAM);if(code)return code;
    return config.topics.enabled?Probe(config.discovery.interface_address,config.topics.port,SOCK_DGRAM):0;
    // Actual ControlNode binds remain authoritative after advisory checks (TOCTOU).
}
}
