#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bringup/bringup.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/publisher.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <dirent.h>
#include <atomic>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
// Test-only linker interception: force real directory flock contention for the
// Supervisor's TryCreate, without changing product code or blocking child startup.
static bool inject_directory_contention=false;
static unsigned injected_attempts=0;
extern "C" int __real_flock(int,int);
extern "C" int __wrap_flock(int fd,int operation){
    struct stat actual{},directory{};
    if(inject_directory_contention && operation==(LOCK_EX|LOCK_NB) &&
       fstat(fd,&actual)==0 && stat("/dev/shm",&directory)==0 &&
       actual.st_dev==directory.st_dev && actual.st_ino==directory.st_ino){
        int held=open("/dev/shm",O_RDONLY|O_DIRECTORY);assert(held>=0);
        assert(__real_flock(held,LOCK_EX)==0);
        const int result=__real_flock(fd,operation);const int error=errno;
        assert(result==-1 && error==EAGAIN);++injected_attempts;
        assert(__real_flock(held,LOCK_UN)==0);close(held);errno=error;return result;
    }
    return __real_flock(fd,operation);
}
using namespace std::chrono_literals;
namespace {
void Send(int fd,int n) { assert(write(fd,&n,sizeof(n))==sizeof(n)); }
int Receive(int fd) { pollfd event{fd,POLLIN,0}; assert(poll(&event,1,10000)==1); int n;assert(read(fd,&n,sizeof(n))==sizeof(n));return n; }
template<class F> void Until(F f) { auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);} }
struct Element: kcf::ProcessElement {
    std::string topic; kcf::Publisher<int> pub;
    explicit Element(std::string t):topic(std::move(t)){}
    int Setup() override { return topic.empty()?0:pub.Create(topic); }
    int Loop() override { return 0; }
    void Shutdown() override { if(!topic.empty()){assert(pub.Close()==0);assert(pub.Unlink()==0);} }
};
bool FindSupervisor(pid_t pid,kcf::SupervisorInfo& result) {
    kcf::IntrospectionClient client;std::vector<kcf::SupervisorInfo> all;
    assert(client.ListSupervisors(all)==0);
    for(const auto& item:all)if(item.pid==pid){result=item;return true;}
    return false;
}
kcf::RuntimeInfo Runtime(std::int32_t pid,std::uint64_t ticks=0) {
    kcf::RuntimeInfo result{};
    Until([&]{kcf::IntrospectionClient c;std::vector<kcf::RuntimeInfo> all;assert(c.ListRuntimes(all)==0);
        for(const auto& r:all)if(r.pid==pid && (!ticks || r.process_start_ticks==ticks) && r.state==kcf::ProcessState::RUNNING){result=r;return true;}return false;});
    return result;
}
void CleanupCrash(const kcf::SupervisorInfo& snapshot) {
    for(std::uint32_t i=0;i<snapshot.element_count;++i){const auto& e=snapshot.elements[i];
        shm_unlink(kcf::detail::RuntimeRegistryName(e.pid,e.process_start_ticks).c_str());
        shm_unlink(kcf::detail::EndpointRegistryName(e.pid,e.process_start_ticks).c_str());}
}
// White-box fixture only: simulate readers paused with slots pinned and the
// legacy notification mutex held. The public Tool API never sees this layout.
using Storage = kcf::detail::ChannelHeader;
using Slot = kcf::detail::ChannelSlot<kcf::SupervisorInfo>;
void Missing(const std::string& name){int fd=shm_open(name.c_str(),O_RDONLY,0);assert(fd<0 && errno==ENOENT);}
unsigned FdCount(){
    auto* d=opendir("/proc/self/fd");assert(d);unsigned count=0;
    while(auto* entry=readdir(d))if(entry->d_name[0]!='.')++count;
    closedir(d);return count;
}
void CreationRecoveryContracts(){
    using Registry=kcf::detail::SupervisorRegistry;using State=Registry::CreationState;
    std::uint64_t ticks=0;assert(kcf::detail::ReadProcessIdentity(getpid(),ticks));
    const auto name=kcf::detail::SupervisorRegistryName(getpid(),ticks);
    const unsigned before=FdCount();
    kcf::SupervisorInfo s{};s.application_state=kcf::ApplicationState::INITIALIZING;
    std::strcpy(s.application_name,"recovery_contract");
    Registry registry;registry.End();
    int fd=open("/dev/shm",O_RDONLY|O_DIRECTORY);assert(fd>=0 && flock(fd,LOCK_EX)==0);
    registry.Begin();assert(registry.GetCreationState()==State::RETRY_PENDING);
    assert(registry.GetCreateAttempts()==1 && registry.GetCreateError()==-EAGAIN);
    registry.Update(s);assert(registry.GetCreateAttempts()==2);Missing(name);
    s.application_state=kcf::ApplicationState::RUNNING;registry.Update(s);
    assert(registry.GetCreateAttempts()==3);Missing(name);
    assert(flock(fd,LOCK_UN)==0);close(fd);
    registry.Update(s);assert(registry.GetCreationState()==State::ACTIVE && registry.GetCreateAttempts()==4);
    kcf::SupervisorInfo found{};assert(FindSupervisor(getpid(),found));
    assert(found.pid==getpid() && found.process_start_ticks==ticks && found.revision==2);
    assert(found.application_state==kcf::ApplicationState::RUNNING);
    registry.Update(s);assert(FindSupervisor(getpid(),found) && found.revision==2);
    registry.End();registry.End();Missing(name);assert(FdCount()==before);
    // Exhaustion is sticky, including repeated Begin. No new creation after release.
    fd=open("/dev/shm",O_RDONLY|O_DIRECTORY);assert(fd>=0 && flock(fd,LOCK_EX)==0);
    registry.Begin();
    for(unsigned attempt=2;attempt<=Registry::MAX_CREATE_ATTEMPTS;++attempt){
        registry.Update(s);assert(registry.GetCreateAttempts()==attempt);
    }
    assert(registry.GetCreationState()==State::EXHAUSTED && registry.GetCreateError()==-EAGAIN);
    assert(flock(fd,LOCK_UN)==0);close(fd);
    registry.Begin();registry.Update(s);Missing(name);
    assert(registry.GetCreateAttempts()==Registry::MAX_CREATE_ATTEMPTS);
    registry.End();assert(FdCount()==before);
    // Cleanup while still pending; End permits an explicit new lifecycle.
    fd=open("/dev/shm",O_RDONLY|O_DIRECTORY);assert(fd>=0 && flock(fd,LOCK_EX)==0);
    registry.Begin();registry.End();assert(flock(fd,LOCK_UN)==0);close(fd);Missing(name);
    registry.Begin();registry.Update(s);assert(FindSupervisor(getpid(),found) && found.revision==1);
    registry.End();Missing(name);assert(FdCount()==before);
    // Permanent collision does not consume repeated attempts or unlink its owner.
    kcf::SharedChannel<kcf::SupervisorInfo> owner;assert(owner.Create(name)==0);
    registry.Begin();assert(registry.GetCreationState()==State::FAILED && registry.GetCreateError()==-EEXIST);
    for(int i=0;i<100;++i)registry.Update(s);
    assert(registry.GetCreateAttempts()==1);registry.End();
    int check=shm_open(name.c_str(),O_RDONLY,0);assert(check>=0);close(check);
    assert(owner.Close()==0 && owner.Unlink()==0);assert(FdCount()==before);
    std::cout<<"PASS creation EAGAIN recovery, latest snapshot/revision, bounded attempts, permanent errors, cleanup/FDs\n";
}
void BringupCreationRecovery(const char* executable){
    const unsigned before=FdCount();
    int command[2],reply[2];assert(pipe(command)==0 && pipe(reply)==0);
    const pid_t supervisor=fork();assert(supervisor>=0);
    if(!supervisor){
        close(command[1]);close(reply[0]);
        inject_directory_contention=true;
        bringup::Bringup app;
        assert(app.Setup("recovery_app",{{"member",executable,{"--child"}}})==0);
        assert(app.GetApplicationState()==kcf::ApplicationState::RUNNING);
        assert(injected_attempts>=2 && injected_attempts<kcf::detail::SupervisorRegistry::MAX_CREATE_ATTEMPTS);
        Send(reply[1],app.GetElementStatuses().front().pid);
        // Parent checks absence and live Runtime before allowing the existing Run loop.
        assert(Receive(command[0])==1);inject_directory_contention=false;
        _exit(app.Run());
    }
    close(command[0]);close(reply[1]);const auto member=Receive(reply[0]);
    kcf::SupervisorInfo info{};assert(!FindSupervisor(supervisor,info));
    const auto runtime=Runtime(member);assert(runtime.execution_mode==kcf::ExecutionMode::SUPERVISED);
    kcf::SystemStatusSubscriber status;std::uint64_t ticks=0;
    assert(kcf::detail::ReadProcessIdentity(supervisor,ticks));assert(status.OpenForApplication(supervisor,ticks)==0);
    kcf::SystemStatus system{};assert(status.ReadCurrent(system)==0 && system.state==kcf::ApplicationState::RUNNING);
    Send(command[1],1);
    Until([&]{return FindSupervisor(supervisor,info) && info.application_state==kcf::ApplicationState::RUNNING;});
    assert(info.pid==supervisor && info.process_start_ticks==ticks && info.element_count==1);
    assert(info.elements[0].pid==member && info.elements[0].process_start_ticks==runtime.process_start_ticks);
    const auto revision=info.revision;
    for(int i=0;i<20;++i)assert(FindSupervisor(supervisor,info) && info.revision==revision);
    assert(kill(supervisor,SIGTERM)==0);int code;assert(waitpid(supervisor,&code,0)==supervisor && WIFEXITED(code) && WEXITSTATUS(code)==0);
    assert(status.Close()==0);close(command[1]);close(reply[0]);
    Missing(kcf::detail::SupervisorRegistryName(supervisor,ticks));assert(FdCount()==before);
    std::cout<<"PASS actual Bringup/Runtime RUNNING while absent, Run Update recovery and membership identity\n";
}

}
int main(int argc,char** argv) {
    if(argc>=2 && std::string(argv[1])=="--child") {
        Element e(argc>2?argv[2]:"");kcf::ProcessRuntime r;r.SetLoopFrequency(500);return r.Run(e)?1:0;
    }
    alarm(90);
    std::string self=kcf::detail::CurrentExecutableBasename();
    char executable[4096]{};auto length=readlink("/proc/self/exe",executable,sizeof(executable)-1);assert(length>0);
    if (argc==3 && std::string(argv[1])=="--cli") {
        pid_t cli=fork();assert(cli>=0);
        if(!cli){execl(argv[2],argv[2],"--application-name","r2b_cli_app",executable,"--child",nullptr);_exit(127);}
        kcf::SupervisorInfo snapshot{};
        Until([&]{return FindSupervisor(cli,snapshot) && snapshot.application_state==kcf::ApplicationState::RUNNING;});
        assert(std::string(snapshot.application_name)=="r2b_cli_app" && snapshot.element_count==1);
        assert(kill(cli,SIGTERM)==0);int status=0;
        assert(waitpid(cli,&status,0)==cli && WIFEXITED(status) && WEXITSTATUS(status)==0);
        std::cout<<"PASS leading --application-name CLI\n";return 0;
    }
    CreationRecoveryContracts();
    BringupCreationRecovery(executable);
    const std::string topic="/r2b_topic_"+std::to_string(getpid());
    pid_t standalone=fork();assert(standalone>=0);
    if(!standalone){unsetenv(kcf::SUPERVISION_FD_ENV);execl(executable,executable,"--child",nullptr);_exit(127);}
    const auto standalone_info=Runtime(standalone);
    assert(standalone_info.execution_mode==kcf::ExecutionMode::STANDALONE);
    int command[2],reply[2];assert(pipe(command)==0 && pipe(reply)==0);
    pid_t supervisor=fork();assert(supervisor>=0);
    if(!supervisor) {
        close(command[1]);close(reply[0]);
        bringup::Bringup app;
        const int setup=app.Setup("r2b_test_app",{{"element_a",executable,{"--child",topic}}, {"element_b",executable,{"--child"}}});
        Send(reply[1],setup);if(setup)_exit(1);
        std::thread requests([&]{for(;;){int n=Receive(command[0]);if(n==9)break;assert(n==1);app.RequestReset();Send(reply[1],1);}});
        int result=app.Run();requests.join();_exit(result);
    }
    close(command[0]);close(reply[1]);assert(Receive(reply[0])==0);
    kcf::SupervisorInfo info{};
    Until([&]{return FindSupervisor(supervisor,info)&&info.application_state==kcf::ApplicationState::RUNNING;});
    assert(std::string(info.application_name)=="r2b_test_app" && info.process_start_ticks && info.element_count==2);
    for(std::uint32_t i=0;i<info.element_count;++i){const auto& member=info.elements[i];
        assert(member.process_alive && member.pid!=standalone && member.process_start_ticks);
        assert(std::string(member.name)==(i==0?"element_a":"element_b"));
        assert(Runtime(member.pid,member.process_start_ticks).execution_mode==kcf::ExecutionMode::SUPERVISED);
    }
    kcf::IntrospectionClient client;std::vector<kcf::EndpointInfo> endpoints;
    assert(client.ListEndpoints(Runtime(info.elements[0].pid,info.elements[0].process_start_ticks),endpoints)==0);
    bool has_topic=false;for(const auto& e:endpoints)if(e.name==topic && e.role==kcf::EndpointRole::PUBLISHER)has_topic=true;
    assert(has_topic);
    const auto old=info;
    std::this_thread::sleep_for(400ms);assert(FindSupervisor(supervisor,info)&&info.revision==old.revision);
    std::cout<<"PASS discovery, membership identity, endpoint chain, standalone separation, stable revision\n";
    const auto name=kcf::detail::SupervisorRegistryName(supervisor,info.process_start_ticks);
    kcf::detail::ChannelLayout layout;assert(kcf::detail::ComputeChannelLayout(sizeof(kcf::SupervisorInfo),alignof(kcf::SupervisorInfo),1,layout));
    int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);struct stat st{};assert(fstat(fd,&st)==0 && static_cast<std::size_t>(st.st_size)==layout.length);
    auto* raw=static_cast<Storage*>(mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(raw!=MAP_FAILED);close(fd);
    auto slot=[&](unsigned i)->Slot&{return *reinterpret_cast<Slot*>(reinterpret_cast<unsigned char*>(raw)+layout.slots+i*layout.stride);};
    int ready[2];assert(pipe(ready)==0);
    pid_t observer=fork();assert(observer>=0);
    if(!observer){close(ready[0]);kcf::SupervisorInfo seen{};for(int i=0;i<20;++i)assert(FindSupervisor(supervisor,seen));
        assert(pthread_mutex_lock(&raw->notify_mutex)==0);
        for(unsigned i=0;i<3;++i)slot(i).users.fetch_add(1);
        Send(ready[1],1);for(;;)pause();}
    close(ready[1]);assert(Receive(ready[0])==1);close(ready[0]);
    // No introspection read while fixture pins are installed. Observe the unchanged
    // safety channel to prove health checks and reset continue despite EAGAIN.
    kcf::SystemStatusSubscriber status;assert(status.OpenForApplication(old.pid,old.process_start_ticks)==0);
    assert(kill(old.elements[0].pid,SIGKILL)==0);
    Until([&]{kcf::SystemStatus s{};return status.ReadCurrent(s)==0 && s.state==kcf::ApplicationState::ERROR;});
    Send(command[1],1);assert(Receive(reply[0])==1);
    Until([&]{kcf::SystemStatus s{};return status.ReadCurrent(s)==0 && s.state==kcf::ApplicationState::RUNNING;});
    assert(kill(observer,SIGKILL)==0);int code;assert(waitpid(observer,&code,0)==observer&&WIFSIGNALED(code));
    // Test-owned abandoned pins are released; dirty snapshot can be retried.
    for(unsigned i=0;i<3;++i)slot(i).users.fetch_sub(1);
    Until([&]{return FindSupervisor(supervisor,info)&&info.application_state==kcf::ApplicationState::RUNNING&&info.elements[0].pid!=old.elements[0].pid;});
    assert(info.revision>old.revision);
    for(std::uint32_t i=0;i<info.element_count;++i){assert(info.elements[i].pid!=old.elements[i].pid);Runtime(info.elements[i].pid,info.elements[i].process_start_ticks);}
    assert(status.Close()==0);assert(munmap(raw,layout.length)==0);
    std::cout<<"PASS pinned/dead observer: health ERROR, Reset, new generations and dirty retry without control blocking\n";
    Send(command[1],9);assert(kill(supervisor,SIGTERM)==0);
    assert(waitpid(supervisor,&code,0)==supervisor&&WIFEXITED(code)&&WEXITSTATUS(code)==0);
    assert(!FindSupervisor(supervisor,info));Missing(name);CleanupCrash(old);
    close(command[1]);close(reply[0]);
    assert(kill(standalone,SIGTERM)==0);assert(waitpid(standalone,&code,0)==standalone&&WIFEXITED(code)&&WEXITSTATUS(code)==0);
    // A killed standalone metadata owner is enough to test stale Supervisor filtering
    // without abandoning managed child processes or SystemStatus ownership.
    assert(pipe(ready)==0);pid_t stale=fork();assert(stale>=0);
    if(!stale){close(ready[0]);kcf::detail::SupervisorRegistry registry;registry.Begin();kcf::SupervisorInfo s{};registry.Update(s);Send(ready[1],1);for(;;)pause();}
    close(ready[1]);assert(Receive(ready[0])==1);close(ready[0]);
    assert(FindSupervisor(stale,info));const auto stale_name=kcf::detail::SupervisorRegistryName(stale,info.process_start_ticks);
    assert(kill(stale,SIGKILL)==0);assert(waitpid(stale,&code,0)==stale);assert(!FindSupervisor(stale,info));
    fd=shm_open(stale_name.c_str(),O_RDONLY,0);assert(fd>=0);close(fd);assert(shm_unlink(stale_name.c_str())==0);
    // Directory flock contention cannot delay the try-create introspection path.
    fd=open("/dev/shm",O_RDONLY|O_DIRECTORY);assert(fd>=0&&flock(fd,LOCK_EX)==0);
    {kcf::detail::SupervisorRegistry registry;auto start=std::chrono::steady_clock::now();registry.Begin();assert(std::chrono::steady_clock::now()-start<500ms);}
    assert(flock(fd,LOCK_UN)==0);close(fd);
    std::cout<<"PASS normal cleanup, stale filtering, nonblocking creation\n";
    // Actual Bringup must still start all 65 configured children, even though
    // only the first 64 are representable. Also exercise registry Create failure.
    for (bool collision : {false, true}) {
        int start[2],done[2]; assert(pipe(start)==0 && pipe(done)==0);
        pid_t owner=fork();assert(owner>=0);
        if(!owner) {
            close(start[1]);close(done[0]);assert(Receive(start[0])==1);
            bringup::Bringup app;std::vector<bringup::ElementSpec> specs;
            for(int i=0;i<(collision?1:65);++i) specs.push_back({"member"+std::to_string(i),executable,{"--child"},10000,3000,1500});
            const int result=app.Setup(std::move(specs));Send(done[1],result);
            if(result)_exit(1);
            _exit(app.Run());
        }
        close(start[0]);close(done[1]);std::uint64_t generation=0;
        assert(kcf::detail::ReadProcessIdentity(owner,generation));
        const auto object=kcf::detail::SupervisorRegistryName(owner,generation);
        kcf::SharedChannel<kcf::SupervisorInfo> blocker;
        if(collision)assert(blocker.Create(object)==0);
        Send(start[1],1);assert(Receive(done[0])==0);
        if(!collision) {
            Until([&]{return FindSupervisor(owner,info) && info.application_state==kcf::ApplicationState::RUNNING;});
            assert(info.element_count==64 && std::string(info.application_name)==self);
            std::vector<kcf::RuntimeInfo> runtimes;assert(client.ListRuntimes(runtimes)==0);
            unsigned supervised=0;for(const auto& r:runtimes)if(r.execution_mode==kcf::ExecutionMode::SUPERVISED && r.state==kcf::ProcessState::RUNNING)++supervised;
            assert(supervised>=65);
        } else assert(!FindSupervisor(owner,info));
        assert(kill(owner,SIGTERM)==0);assert(waitpid(owner,&code,0)==owner && WIFEXITED(code) && WEXITSTATUS(code)==0);
        if(collision){assert(blocker.Close()==0);assert(blocker.Unlink()==0);}
        Missing(object);close(start[1]);close(done[0]);
    }
    std::cout<<"PASS capacity 65, fallback name, metadata creation failure isolation\nR2B supervisor discovery PASS\n";
}
