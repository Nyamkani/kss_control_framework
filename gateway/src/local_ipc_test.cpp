#include "local_ipc.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
namespace fs=std::filesystem;
using namespace std::chrono_literals;
namespace l=gateway::local;
int Connect(const std::string& path){int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);timeval tv{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));sockaddr_un a{};a.sun_family=AF_UNIX;std::copy(path.begin(),path.end(),a.sun_path);assert(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));return fd;}
std::string Query(const std::string& path,unsigned op){gateway::Fd fd;fd.value=Connect(path);auto f=l::Frame(op,0);assert(send(fd.value,f.data(),f.size(),MSG_NOSIGNAL)==16);std::string all;char bytes[4096];ssize_t n;while((n=recv(fd.value,bytes,sizeof(bytes),0))>0)all.append(bytes,n);assert(n==0&&all.size()>=16&&l::Get(all.data(),4)==all.size()-16);return all;}
std::size_t Fds(){return std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator());}
int main(int argc,char** argv){
 if(argc==2&&std::string(argv[1])=="--peer") {
    namespace n=kcf::network;n::DiscoveryConfig c;c.host.bytes[0]=77;c.boot_id.bytes[0]=78;
    c.interface_address="192.0.2.1";c.port=47651;c.announce_interval=50ms;
    n::NetworkEndpointMetadata e;e.identity={{c.host,c.boot_id,getpid(),1},1};e.host_name="test-peer";e.element_name="test-element";e.name="/ipc/peer";e.value_type={1,2,1,8};
    n::PeerDiscovery peer;assert(!peer.Start(c,{e}));std::cout<<"ready"<<std::endl;
    std::string line;while(std::getline(std::cin,line)&&line!="quit"){if(line=="remove")assert(!peer.UpdateMetadata({}));std::cout<<"ok"<<std::endl;}
    peer.Stop();return 0;
 }
char temp[]="/tmp/kcf-local-ipc-XXXXXX";assert(mkdtemp(temp));std::string root=temp,path=root+"/gateway.sock";const auto baseline=Fds();
 ucred allowed{};allowed.uid=geteuid();assert(l::SameUid(allowed,geteuid()));++allowed.uid;assert(!l::SameUid(allowed,geteuid()));
 {
 l::Server server;l::Status s;s.host.bytes[0]=1;s.boot.bytes[0]=2;server.Publish(s,{});assert(!server.Start(path));
 struct stat st{};assert(!lstat(path.c_str(),&st)&&(st.st_mode&0777)==0600);
 assert(Query(path,l::GET_STATUS).find("WAITING_NETWORK")!=std::string::npos);
 l::Server duplicate;assert(duplicate.Start(path)==-EBUSY);
 for(auto state:{gateway::NetworkState::REBINDING,gateway::NetworkState::RUNNING,gateway::NetworkState::ERROR}){s.state=state;++s.session;server.Publish(s,{});assert(Query(path,l::GET_STATUS).find(gateway::StateName(state))!=std::string::npos);}
 kcf::network::PeerInfo peer;peer.host.bytes[0]=3;peer.boot_id.bytes[0]=4;peer.session_id=42;peer.metadata_revision=2;peer.metadata_ready=true;
 kcf::network::NetworkEndpointMetadata e;e.name="/quote\"\\\n";peer.endpoints={e};server.Publish(s,{peer});auto data=Query(path,l::GET_PEERS);assert(data.find("\\\"")!=std::string::npos&&data.find("\"metadata_revision\":\"2\"")!=std::string::npos);
 peer.metadata_revision=3;peer.endpoints.clear();server.Publish(s,{peer});assert(Query(path,l::GET_PEERS).find("\"metadata_revision\":\"3\"")!=std::string::npos);
 peer.endpoints={e};peer.endpoints[0].name=std::string(l::MAX_BODY,'a');server.Publish(s,{peer});
 auto large=Query(path,l::GET_PEERS);assert(static_cast<std::int32_t>(l::Get(large.data()+12,4))==-E2BIG&&large.size()==16);
 server.Publish(s,{});assert(Query(path,l::GET_PEERS).substr(16)=="{\"peers\":[]}");
 auto unsupported=Query(path,99);assert(static_cast<std::int32_t>(l::Get(unsupported.data()+12,4))==-ENOTSUP);
 // Malformed/oversized/version inputs close without allocating a body.
 for(int variant=0;variant<4;++variant){gateway::Fd fd;fd.value=Connect(path);auto f=l::Frame(l::GET_STATUS,0);f[variant==0?0:variant==1?4:variant==2?9:15]=char(255);assert(send(fd.value,f.data(),16,MSG_NOSIGNAL)==16);char c;assert(recv(fd.value,&c,1,0)==0);}
 {gateway::Fd fd;fd.value=Connect(path);assert(send(fd.value,"x",1,MSG_NOSIGNAL)==1);} // partial disconnect
 {gateway::Fd fd;fd.value=Connect(path);char c;assert(recv(fd.value,&c,1,0)==0);} // bounded incomplete request
 {gateway::Fd fd;fd.value=Connect(path);auto f=l::Frame(l::GET_STATUS,0);assert(send(fd.value,f.data(),8,MSG_NOSIGNAL)==8);
 std::this_thread::sleep_for(60ms);assert(send(fd.value,f.data()+8,8,MSG_NOSIGNAL)==8);char h[16];assert(recv(fd.value,h,16,MSG_WAITALL)==16&&l::Get(h+4,4)==l::MAGIC);}

 std::this_thread::sleep_for(1100ms); // reset fixed admission window, not Network readiness
 std::vector<int> pending;for(int i=0;i<8;++i)pending.push_back(Connect(path));
 std::this_thread::sleep_for(100ms);gateway::Fd excess;excess.value=Connect(path);char c;assert(recv(excess.value,&c,1,0)==0);for(int fd:pending)close(fd);
 std::this_thread::sleep_for(100ms);assert(Query(path,l::GET_STATUS).size()>16);
 server.Stop();server.Stop();assert(!fs::exists(path));
 assert(!server.Start(path));server.Stop();
 }
 assert(Fds()==baseline);
 // Crash leaves a stale socket; a new owner safely reclaims after flock release.
 auto child=fork();assert(child>=0);if(!child){l::Server server;if(server.Start(path))_exit(2);_exit(0);}
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(fs::exists(path));
 {l::Server server;assert(!server.Start(path));server.Stop();}
 assert(!symlink("target",path.c_str()));{l::Server server;assert(server.Start(path)==-EACCES);}assert(fs::is_symlink(path));unlink(path.c_str());
 fs::remove_all(root);assert(Fds()==baseline);
 std::cout<<"Local IPC snapshot/states/revision/frame bounds/8 clients/restart/stale/symlink/FD PASS; cross-UID credentials require multi-UID environment\n";
}
