#pragma once
#include "gateway_support.hpp"
#include "network_lifecycle.hpp"
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/un.h>

namespace gateway::local {
// Local-only v1: 16-byte big-endian header, see gateway/LOCAL_IPC.md.
inline constexpr std::uint32_t MAGIC=0x4b43464c, MAX_BODY=1024*1024;
inline constexpr std::size_t MAX_CLIENTS=8;
inline bool SameUid(const ucred& peer,uid_t owner){return peer.uid==owner;}
inline constexpr std::uint16_t VERSION=1, GET_STATUS=1, GET_PEERS=2;
inline void Put(std::string& s,std::uint64_t v,unsigned n){for(unsigned i=n;i;--i)s+=char(v>>((i-1)*8));}
inline std::uint32_t Get(const char* p,unsigned n){std::uint32_t v=0;while(n--)v=(v<<8)|static_cast<unsigned char>(*p++);return v;}
inline std::string Frame(std::uint16_t op,int error,const std::string& body={}) {
    std::string s;Put(s,body.size(),4);Put(s,MAGIC,4);Put(s,VERSION,2);Put(s,op,2);Put(s,static_cast<std::uint32_t>(error),4);s+=body;return s;
}
// JSON numbers carrying 64-bit identities are decimal strings (no JS rounding).
inline std::string Quote(const std::string& s){std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32||c>=127){out+="\\u00";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}else out+=c;}return out+'"';}
inline std::string Hex(const kcf::network::HostIdentity& id){std::string s;for(auto b:id.bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
inline std::string Number(std::uint64_t v){return Quote(std::to_string(v));}
inline std::string Runtime(const kcf::network::RemoteRuntimeIdentity& r){return "{\"host\":"+Quote(Hex(r.host))+",\"boot\":"+Quote(Hex(r.boot_id))+",\"pid\":"+std::to_string(r.pid)+",\"start_ticks\":"+Number(r.process_start_ticks)+"}";}
inline std::string Type(const kcf::network::TypeIdentity& t){return "{\"type_id\":"+Number(t.type_id)+",\"schema_id\":"+Number(t.schema_id)+",\"encoding_id\":"+std::to_string(t.encoding_id)+",\"payload_size\":"+std::to_string(t.payload_size)+"}";}
inline std::string Peers(const std::vector<kcf::network::PeerInfo>& peers) {
    std::string out="{\"peers\":[";bool first=true;
    for(const auto& p:peers){if(!first)out+=',';first=false;
        out+="{\"host\":"+Quote(Hex(p.host))+",\"boot\":"+Quote(Hex(p.boot_id))+",\"session\":"+Number(p.session_id)+",\"state\":"+Quote(p.state==kcf::network::PeerState::ONLINE?"ONLINE":"LOST")+",\"address\":"+Quote(p.address)+",\"metadata_port\":"+std::to_string(p.port)+",\"metadata_revision\":"+Number(p.metadata_revision)+",\"metadata_ready\":"+(p.metadata_ready?"true":"false")+",\"endpoints\":[";
        bool endpoint_first=true;for(const auto& e:p.endpoints){if(!endpoint_first)out+=',';endpoint_first=false;
            out+="{\"runtime\":"+Runtime(e.identity.runtime)+",\"registration_id\":"+Number(e.identity.registration_id)+",\"application\":"+Runtime(e.application)+",\"host_name\":"+Quote(e.host_name)+",\"application_name\":"+Quote(e.application_name)+",\"element_name\":"+Quote(e.element_name)+",\"name\":"+Quote(e.name)+",\"kind\":"+std::to_string(int(e.kind))+",\"role\":"+std::to_string(int(e.role))+",\"scope\":"+std::to_string(int(e.scope))+",\"value_type\":"+Type(e.value_type)+",\"request_type\":"+Type(e.request_type)+",\"response_type\":"+Type(e.response_type)+",\"feedback_type\":"+Type(e.feedback_type)+",\"groups\":[";
            bool group_first=true;for(const auto& g:e.groups){if(!group_first)out+=',';group_first=false;out+=Quote(g);}out+="]}";
            if(out.size()>MAX_BODY)throw std::length_error("Local peer snapshot too large");
        }out+="]}";
    }out+="]}";if(out.size()>MAX_BODY)throw std::length_error("Local peer snapshot too large");return out;
}
struct Status {
    NetworkState state{NetworkState::WAITING_NETWORK};
    kcf::network::HostIdentity host,boot;
    kcf::network::NetworkInterfaceSelection nic;
    std::uint64_t session{0};int error{0};
    std::string Json() const {return "{\"state\":"+Quote(StateName(state))+",\"host\":"+Quote(Hex(host))+",\"boot\":"+Quote(Hex(boot))+",\"interface\":"+Quote(nic.name)+",\"ipv4\":"+Quote(nic.ipv4_address)+",\"interface_index\":"+std::to_string(nic.index)+",\"session\":"+Number(session)+",\"last_error\":"+std::to_string(error)+"}";}
};
// Start/Stop caller-serialized. Publish copies snapshots; worker never touches
// ControlNode/Lifecycle. One request per connection, bounded deadlines/resources.
class Server {
public:
    ~Server(){Stop();}
    int Start(const std::string& path) {
        if(listener_>=0)return -EALREADY;
        if(path.empty()||path.size()>=sizeof(sockaddr_un::sun_path))return -ENAMETOOLONG;
        int code=Lock(lock_,path+".lock");if(code){ReleaseLock();return code;}
        path_=path;struct stat st{};
        if(!lstat(path.c_str(),&st)){
            if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()){ReleaseLock();return -EACCES;}
            Fd probe;probe.value=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
            if(probe.value<0){code=-errno;ReleaseLock();return code;}
            auto a=Address(path);
            if(connect(probe.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0||errno!=ECONNREFUSED){ReleaseLock();return -EADDRINUSE;}
            if(unlink(path.c_str())){code=-errno;ReleaseLock();return code;}
        }else if(errno!=ENOENT){code=-errno;ReleaseLock();return code;}
        listener_=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(listener_<0){code=-errno;ReleaseLock();return code;}
        auto a=Address(path);
        if(bind(listener_,reinterpret_cast<sockaddr*>(&a),sizeof(a))){code=-errno;Stop();return code;}
        if(lstat(path.c_str(),&owned_)){code=-errno;Stop();return code;}owned_valid_=true;
        if(chmod(path.c_str(),0600)||listen(listener_,MAX_CLIENTS)){code=-errno;Stop();return code;}
        stop_=false;error_=0;
        try{worker_=std::thread([this]{try{Run();}catch(...){error_=-EFAULT;}});}catch(...){Stop();return -EAGAIN;}return 0;
    }
    void Publish(const Status& s,const std::vector<kcf::network::PeerInfo>& peers) {
        auto status=s.Json();std::string data;int error=0;
        try{data=Peers(peers);}catch(const std::length_error&){error=-E2BIG;}
        std::lock_guard<std::mutex> guard(mutex_);status_=std::move(status);peers_=std::move(data);peers_error_=error;
    }
    void Stop(){stop_=true;if(worker_.joinable())worker_.join();
        if(listener_>=0){close(listener_);listener_=-1;}
        if(owned_valid_){struct stat st{};if(!lstat(path_.c_str(),&st)&&st.st_dev==owned_.st_dev&&st.st_ino==owned_.st_ino)unlink(path_.c_str());owned_valid_=false;}ReleaseLock();}
    int GetLastError()const{return error_;}
private:
    using Clock=std::chrono::steady_clock;
    struct Client {Fd fd;std::array<char,16> header{};std::size_t read{0},sent{0};std::string output;Clock::time_point deadline;};
    static sockaddr_un Address(const std::string& p){sockaddr_un a{};a.sun_family=AF_UNIX;std::copy(p.begin(),p.end(),a.sun_path);return a;}
    void ReleaseLock(){if(lock_.value>=0){close(lock_.value);lock_.value=-1;}}
    bool Step(Client& c) {
        if(Clock::now()>=c.deadline)return false;
        if(c.read<16){auto n=recv(c.fd.value,c.header.data()+c.read,16-c.read,MSG_DONTWAIT);
            if(n==0)return false;
            if(n<0)return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;
            c.read+=n;if(c.read!=16)return true;const auto* h=c.header.data();
            if(Get(h,4)!=0||Get(h+4,4)!=MAGIC||Get(h+8,2)!=VERSION||Get(h+12,4)!=0)return false;
            const auto op=Get(h+10,2);std::lock_guard<std::mutex> guard(mutex_);
            c.output=op==GET_STATUS?Frame(op,0,status_):op==GET_PEERS?Frame(op,peers_error_,peers_):Frame(op,-ENOTSUP);
        }
        auto n=send(c.fd.value,c.output.data()+c.sent,c.output.size()-c.sent,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0)return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;
        c.sent+=n;return c.sent<c.output.size();
    }
    void Run() {
        std::vector<std::unique_ptr<Client>> clients;auto window=Clock::now();unsigned admissions=0;
        while(!stop_){
            auto now=Clock::now();if(now-window>=std::chrono::seconds(1)){window=now;admissions=0;}
            pollfd fds[1+MAX_CLIENTS]{};fds[0]={listener_,short(admissions<32?POLLIN:0),0};
            for(std::size_t i=0;i<clients.size();++i)fds[i+1]={clients[i]->fd.value,short(clients[i]->read==16?POLLOUT:POLLIN),0};
            int result=poll(fds,clients.size()+1,50);if(result<0){if(errno==EINTR)continue;error_=-errno;return;}
            for(std::size_t i=clients.size();i--;){if(!Step(*clients[i]))clients.erase(clients.begin()+i);}
            if(fds[0].revents&POLLIN)for(unsigned i=0;i<8&&admissions<32;++i){
                auto c=std::make_unique<Client>();c->fd.value=accept4(listener_,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
                if(c->fd.value<0)break;
                ++admissions;
                ucred cred{};socklen_t size=sizeof(cred);
                if(clients.size()>=MAX_CLIENTS||getsockopt(c->fd.value,SOL_SOCKET,SO_PEERCRED,&cred,&size)||size!=sizeof(cred)||!SameUid(cred,geteuid()))continue;
                c->deadline=Clock::now()+std::chrono::seconds(2);clients.push_back(std::move(c));
            }
        }
    }
    Fd lock_;int listener_{-1};std::string path_;struct stat owned_{};bool owned_valid_{false};
    std::atomic<bool> stop_{false};std::atomic<int> error_{0};std::thread worker_;std::mutex mutex_;
    std::string status_{"{}"},peers_{"{\"peers\":[]}"};int peers_error_{0};
};
}
