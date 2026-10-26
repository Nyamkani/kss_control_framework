#include "gateway_support.hpp"
#include "kcf/network/host_identity.hpp"
#include "kcf/network/remote_control_client.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char** environ;
namespace n=kcf::network;
namespace fs=std::filesystem;
using namespace std::chrono_literals;
static std::string Read(const fs::path& path){std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};}
static bool Until(const std::function<bool()>& predicate) {
    auto end=std::chrono::steady_clock::now()+8s;
    do{if(predicate())return true;std::this_thread::sleep_for(50ms);}while(std::chrono::steady_clock::now()<end);
    return false;
}
struct Child {
    pid_t pid{-1};
    Child(const std::vector<std::string>& args,const fs::path& log) {
        std::vector<char*> argv;for(const auto& s:args)argv.push_back(const_cast<char*>(s.c_str()));argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;assert(posix_spawn_file_actions_init(&actions)==0);
        assert(posix_spawn_file_actions_addopen(&actions,1,log.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0600)==0);
        assert(posix_spawn_file_actions_adddup2(&actions,1,2)==0);
        int code=posix_spawn(&pid,argv[0],&actions,nullptr,argv.data(),environ);
        posix_spawn_file_actions_destroy(&actions);assert(code==0);
    }
    int Wait(){int status=0;bool done=Until([&]{return waitpid(pid,&status,WNOHANG)==pid;});assert(done);pid=-1;return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);}
    int Stop(int signal=SIGTERM){assert(kill(pid,signal)==0);return Wait();}
    ~Child(){if(pid>0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}}
};
static void Unit() {
    const auto parse=[](std::vector<std::string> args){std::vector<char*> a;for(auto& s:args)a.push_back(s.data());return gateway::Parse(a.size(),a.data());};
    const auto defaults=parse({"gateway"});assert(defaults.state_dir=="/var/lib/kcf"&&defaults.discovery==37651&&defaults.control==39001&&defaults.topic==37652);
    for(auto args:std::vector<std::vector<std::string>>{{"x","--control-port","0"},{"x","--control-port","65536"},{"x","--control-port","-1"},{"x","--control-port","37651"},{"x","--interface"},{"x","--unknown"},{"x","--allow-loopback"}}){bool rejected=false;try{parse(args);}catch(...){rejected=true;}assert(rejected);}
    auto options=parse({"x","--state-dir","/tmp/state","--interface","test-nic","--control-port","39002"});
    n::NetworkInterfaceInfo one{"test-nic","192.0.2.1",1,true,true,true,false,false};
    n::NetworkInterfaceSelection selected;assert(n::SelectNetworkInterface({one},{},selected)==0&&selected.automatic);
    auto two=one;two.name="second";two.index=2;two.ipv4_address="192.0.2.2";
    assert(n::SelectNetworkInterface({one,two},{},selected)==-ENOTUNIQ);
    assert(n::SelectNetworkInterface({one,two},options.nic,selected)==0&&!selected.automatic);
    auto config=gateway::Config(options,selected);
    assert(config.discovery.interface_address==one.ipv4_address&&config.discovery.metadata_port==39002);
    assert(config.metadata.runtime_pids.empty()&&config.metadata.remote_topics.empty()&&config.metadata.remote_parameters.empty()&&config.metadata.remote_services.empty()&&config.metadata.remote_actions.empty());
    assert(config.parameter_get_clients.empty()&&config.parameter_set_clients.empty()&&config.service_call_clients.empty()&&config.action_goal_clients.empty()&&config.action_cancel_clients.empty()&&config.descriptor_query_clients.empty());
    assert(!config.topics.enabled&&!config.diagnostics_enabled);
    std::cout<<"Gateway configuration/auto-selection/ambiguity/override/default deny PASS\n";
}
static void LockTests() {
    char temp[]="/tmp/kcf-lock-test-XXXXXX";assert(mkdtemp(temp));fs::path root=temp;
    const auto path=(root/"gateway.lock").string();
    {
        gateway::Fd one;assert(gateway::Lock(one,path)==0);
        gateway::Fd two;assert(gateway::Lock(two,path)==-EBUSY);
    }
    {gateway::Fd retry;assert(gateway::Lock(retry,path)==0);}
    fs::create_symlink(path,root/"link");
    {gateway::Fd fd;assert(gateway::Lock(fd,(root/"link").string())==-ELOOP);}
    fs::create_directory_symlink(root,root/"dirlink");
    {gateway::Fd fd;assert(gateway::Lock(fd,(root/"dirlink"/"new").string())==-ELOOP);}
    assert(chmod(path.c_str(),0666)==0);
    {gateway::Fd fd;assert(gateway::Lock(fd,path)==-EACCES);}
    assert(chmod(path.c_str(),0600)==0);
    fs::create_hard_link(path,root/"hard");
    {gateway::Fd fd;assert(gateway::Lock(fd,path)==-EACCES);}
    fs::remove(root/"hard");
    assert(chmod(root.c_str(),0777)==0);
    {gateway::Fd fd;assert(gateway::Lock(fd,path)==-EACCES);}
    assert(chmod(root.c_str(),0700)==0);
    fs::remove_all(root);std::cout<<"Secure runtime lock validation/reacquire PASS\n";
}
static void Network(const std::string& executable) {
    char temp[]="/tmp/kcf-gateway-test-XXXXXX";assert(mkdtemp(temp));fs::path root=temp;
    const auto fd_count=[] {std::size_t n=0;for(const auto& e:fs::directory_iterator("/proc/self/fd")){(void)e;++n;}return n;};
    auto baseline=fd_count();
    std::vector<std::string> args{executable,"--state-dir",root.string(),"--ipc-socket",(root/"gateway.sock").string(),"--interface","lo","--allow-loopback","--discovery-port","47651","--control-port","49001","--topic-port","47652","--lock-file",(root/"gateway.lock").string()};
    // This branch explicitly runs in an isolated network/IPC/mount namespace.
    Child first(args,root/"first.log");assert(Until([&]{return Read(root/"first.log").find("[Gateway] ready")!=std::string::npos;}));
    n::HostIdentity host;assert(n::LoadHostIdentity((root/"host-id").string(),host)==0);
    const auto stored=Read(root/"host-id");
    Child duplicate(args,root/"duplicate.log");assert(duplicate.Wait()==1);
    assert(Read(root/"duplicate.log").find("singleton lock error=-16")!=std::string::npos);
    fs::create_directory(root/"alternate");auto alternate=args;alternate[2]=(root/"alternate").string();alternate[9]="49002";
    Child duplicate_other_state(alternate,root/"duplicate-other.log");assert(duplicate_other_state.Wait()==1);
    assert(Read(root/"duplicate-other.log").find("singleton lock error=-16")!=std::string::npos);
    assert(!fs::exists(root/"alternate"/"host-id"));
    // Keep a separate Local Runtime and Parameter alive through Gateway restarts.
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> parameter;const std::string name="/gateway/test/value";
    assert(parameter.Create(name,{7,true,1.5})==0);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    n::DiscoveryConfig dc;dc.host.bytes[0]=42;assert(n::ReadLinuxBootIdentity(dc.boot_id)==0);
    dc.interface_address="127.0.0.1";dc.port=47651;
    n::PeerDiscovery observer;assert(observer.Start(dc)==0);
    n::PeerInfo peer;n::NetworkEndpointMetadata endpoint;
    assert(Until([&]{for(const auto& p:observer.GetPeers())if(p.host==host&&p.metadata_ready)for(const auto& e:p.endpoints)if(e.name==name){peer=p;endpoint=e;return true;}return false;}));
    assert(endpoint.scope==n::NetworkScope::LOCAL&&endpoint.identity.runtime.pid==getpid());
    assert(!(peer.profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_TOPIC)));
    assert(!(peer.profile.capabilities&n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS)));
    n::RemoteControlClient client(observer,dc);assert(client.Start()==0);
    Value changed{99,false,2.5};kcf::DynamicPayload native;
    const auto descriptor=kcf::TypeDescriptorTraits<Value>::Get();native.type_id=descriptor.type_id;
    native.bytes.resize(sizeof(changed));std::memcpy(native.bytes.data(),&changed,sizeof(changed));
    std::vector<std::uint8_t> bytes;assert(n::EncodeParameterValue(descriptor,native,bytes)==0);
    auto denied=client.SetParameter(endpoint.identity,peer.session_id,name,endpoint.value_type,bytes,1s);
    std::cerr<<"Default Set refusal error="<<denied.error<<" unknown="<<denied.execution_unknown<<'\n';
    assert(denied.error==-EACCES&&!denied.execution_unknown);
    client.Stop();observer.Stop();
    Value value;assert(parameter.Get(value)==0&&value.count==7);
    assert(first.Stop()==0);
    assert(parameter.Get(value)==0&&value.count==7);
    Child second(args,root/"second.log");assert(Until([&]{return Read(root/"second.log").find("[Gateway] ready")!=std::string::npos;}));
    assert(Until([&]{return Read(root/"second.log").find("[Gateway] session=")!=std::string::npos;}));
    assert(Read(root/"host-id")==stored);assert(second.Stop(SIGKILL)==128+SIGKILL);
    Child third(args,root/"third.log");assert(Until([&]{return Read(root/"third.log").find("[Gateway] ready")!=std::string::npos;}));
    assert(Until([&]{return Read(root/"third.log").find("[Gateway] session=")!=std::string::npos;}));
    const auto session=[](const std::string& text){auto pos=text.find("[Gateway] session=");assert(pos!=std::string::npos);return std::stoull(text.substr(pos+18));};
    assert(session(Read(root/"second.log"))>0&&session(Read(root/"third.log"))>0);
    assert(session(Read(root/"first.log"))!=session(Read(root/"second.log")));
    assert(session(Read(root/"second.log"))!=session(Read(root/"third.log")));
    assert(Read(root/"host-id")==stored);assert(third.Stop(SIGINT)==0);
    assert(parameter.Get(value)==0&&value.count==7);
    assert(parameter.Close()==0);assert(parameter.Unlink()==0);
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    // Held TCP or Metadata UDP socket is rejected before node startup.
    for(int type:{SOCK_STREAM,SOCK_DGRAM}) {
        gateway::Fd busy;busy.value=socket(AF_INET,type|SOCK_CLOEXEC,0);assert(busy.value>=0);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(49001);inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
        int one=1;assert(setsockopt(busy.value,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))==0);
        assert(bind(busy.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
        if(type==SOCK_STREAM)assert(listen(busy.value,1)==0);
        Child conflict(args,root/"conflict.log");assert(conflict.Wait()==1);
        assert(Read(root/"conflict.log").find("port preflight error=")!=std::string::npos);
    }
    auto bad=args;bad[2]=(root/"missing").string();Child missing(bad,root/"missing.log");assert(missing.Wait()==1);
    Child final(args,root/"final.log");assert(Until([&]{return Read(root/"final.log").find("[Gateway] ready")!=std::string::npos;}));assert(final.Stop()==0);
    assert(fd_count()==baseline);
    assert(gateway::Probe("127.0.0.1",49001,SOCK_STREAM)==0&&gateway::Probe("127.0.0.1",49001,SOCK_DGRAM)==0);
    gateway::Fd available;assert(gateway::Lock(available,(root/"gateway.lock").string())==0);
    fs::remove_all(root);
    std::cout<<"Gateway isolated lifecycle/metadata/LOCAL deny/identity reuse/crash/ports/FD/lock PASS\n";
}
static void TopicMetadataCLI(const std::string& executable) {
    char temp[]="/tmp/kcf-cli-policy-XXXXXX";assert(mkdtemp(temp));fs::path root=temp;
    std::vector<std::string> args{executable,"--state-dir",root.string(),"--ipc-socket",(root/"gateway.sock").string(),"--lock-file",(root/"gateway.lock").string(),
        "--interface","lo","--allow-loopback","--discovery-port","49651","--control-port","49021","--topic-port","49652",
        "--topic-allow-prefix","/cli","--topic-allow-prefix","/other","--topic-exclude-prefix","/cli/private"};
    Child gateway(args,root/"gateway.log");assert(Until([&]{return Read(root/"gateway.log").find("[Gateway] ready")!=std::string::npos;}));
    n::HostIdentity host;assert(n::LoadHostIdentity((root/"host-id").string(),host)==0);
    n::DiscoveryConfig dc;dc.host.bytes[0]=99;assert(n::ReadLinuxBootIdentity(dc.boot_id)==0);dc.interface_address="127.0.0.1";dc.port=49651;
    n::PeerDiscovery observer;assert(observer.Start(dc)==0);
    const auto peer=[&]{for(const auto& p:observer.GetPeers())if(p.host==host)return p;return n::PeerInfo{};};
    assert(Until([&]{return peer().metadata_ready;}));
    assert(peer().endpoints.empty());assert(peer().profile.capabilities&n::CapabilityBit(n::Capability::REMOTE_TOPIC));
    assert(peer().profile.capabilities&n::CapabilityBit(n::Capability::TYPE_DESCRIPTOR_QUERY));
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    kcf::Publisher<Value> publisher,excluded;assert(publisher.Create("/cli/value")==0);assert(excluded.Create("/cli/private/value")==0);
    kcf::Subscriber<Value> subscriber;assert(subscriber.Create("/cli/value",[](const Value&){})==0);
    assert(Until([&]{return peer().metadata_ready&&peer().endpoints.size()==3;}));
    for(const auto& e:peer().endpoints){const bool allowed=e.name=="/cli/value"&&e.role==n::EndpointRole::PROVIDER;assert(e.scope==(allowed?n::NetworkScope::REMOTE:n::NetworkScope::LOCAL));}
    assert(subscriber.Close()==0);assert(publisher.Close()==0);assert(publisher.Unlink()==0);assert(excluded.Close()==0);assert(excluded.Unlink()==0);
    assert(Until([&]{return peer().metadata_ready&&peer().endpoints.empty();}));
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    observer.Stop();assert(gateway.Stop()==0);fs::remove_all(root);
    std::cout<<"Actual kcf_gateway CLI policy dynamic Metadata/Publisher-only/public Topic Descriptor capability PASS\n";
}
static volatile std::sig_atomic_t owner_stop=0;
static void OwnerSignal(int){owner_stop=1;}
static void LocalOwner() {
    std::signal(SIGTERM,OwnerSignal);std::signal(SIGINT,OwnerSignal);
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::Parameter<Value> parameter;assert(parameter.Create("/gateway/recovery/owner",{7,true,1.5})==0);
    kcf::Publisher<Value> publisher;kcf::Subscriber<Value> subscriber;
    assert(publisher.Create("/gateway/recovery/local-topic")==0);
    assert(subscriber.Create("/gateway/recovery/local-topic")==0);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    std::cout<<"Local owner ready pid="<<getpid()<<std::endl;
    std::uint64_t commits=0;
    while(!owner_stop){
        Value value;assert(parameter.Get(value)==0&&value.count==7);
        assert(publisher.Publish(value)==0);
        Value received{};kcf::TopicReadInfo info;
        assert(subscriber.ReadLatest(received,info)==0&&info.sequence==++commits);
        assert(received.count==7&&received.enabled&&received.gain==1.5);
        std::this_thread::sleep_for(100ms);
    }
    assert(commits>0);
    assert(subscriber.Close()==0);assert(publisher.Close()==0);assert(publisher.Unlink()==0);
    std::cout<<"Local Topic maintained commits="<<commits<<" across Gateway recovery PASS\n";
    assert(parameter.Close()==0);assert(parameter.Unlink()==0);
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);
    std::cout<<"Local owner maintained value and exited normally PASS\n";
}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--owner"){LocalOwner();return 0;}Unit();LockTests();if(argc==3&&std::string(argv[1])=="--policy-network"){TopicMetadataCLI(argv[2]);return 0;}if(argc==3&&std::string(argv[1])=="--network")Network(argv[2]);else assert(argc==1);}
