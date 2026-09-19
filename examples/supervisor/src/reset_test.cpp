#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bringup/bringup.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include "kcf/system/system_status_channel.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <functional>
#include <iostream>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using State = kcf::ApplicationState;
static std::string Read(const fs::path& path) { std::ifstream f(path); std::string s; f >> s; return s; }
static void Write(const fs::path& path, const std::string& value) { std::ofstream f(path); f << value << std::endl; }
static void Until(const std::function<bool()>& predicate, int ms=8000)
{
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    while (!predicate()) { assert(std::chrono::steady_clock::now()<end); std::this_thread::sleep_for(10ms); }
}
static std::size_t Fds() { return std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator{}); }
class Fixture : public kcf::ProcessElement
{
public:
    fs::path root; std::string role, topic;
    kcf::SharedChannel<pid_t> channel;
    kcf::SharedParameter<pid_t> parameter;
    int Setup() override
    {
        prctl(PR_SET_DUMPABLE,0);
        Write(root/(role+".pid"),std::to_string(getpid()));
        const auto mode=Read(root/(role+".mode"));
        if (mode=="fail") return 7;
        if (mode=="delay") std::this_thread::sleep_for(700ms);
        if (mode=="hang") for (;;) std::this_thread::sleep_for(1s);
        if (role=="B")
        {
            if (mode=="incomplete")
            {
                // Test-only creator prefix: leave current Topic/Parameter headers
                // initialized=0 with exclusive flock held until controller SIGKILL.
                std::string encoded="/";
                for(std::size_t i=1;i<topic.size();++i)
                    encoded += topic[i]=='/' ? "%2F" : std::string(1,topic[i]);
                const int topic_fd=kcf::detail::CreateOwnedShm(encoded.c_str(),
                    0x4b4346545249504cULL,sizeof(pid_t),alignof(pid_t),false,
                    kcf::detail::TOPIC_STORAGE_FORMAT);
                const int parameter_fd=kcf::detail::CreateOwnedShm((encoded+"%2Fparameter").c_str(),
                    0x4b4346504152414dULL,sizeof(pid_t),alignof(pid_t));
                assert(topic_fd>=0 && parameter_fd>=0);
                Write(root/"incomplete.ready",std::to_string(getpid()));
                for(;;) pause();
            }
            const int created=channel.Create(topic);
            return created ? created : parameter.Create(topic+"/parameter",getpid());
        }
        int result=-ENOENT;
        for (int i=0;i<200 && result; ++i)
        { result=channel.Open(topic); if(result)std::this_thread::sleep_for(10ms); }
        for (int i=0;i<200;++i)
        {
            const int opened=parameter.Open(topic+"/parameter");
            if(!opened)return result;
            std::this_thread::sleep_for(10ms);
        }
        return -ETIMEDOUT;
    }
    int Loop() override
    {
        if (role=="B")
        {
            if (Read(root/"stall")=="yes") for (;;) std::this_thread::sleep_for(1s);
            channel.Publish(getpid());
        }
        else { pid_t value=0; std::uint32_t seq=0; if(channel.ReadLatestSnapshot(value,seq)==0)Write(root/"A.observed",std::to_string(value)); }
        return 0;
    }
    void Shutdown() override { channel.Close(); parameter.Close(); if(role=="B") { channel.Unlink(); parameter.Unlink(); } }
};
int main(int argc,char** argv)
{
    if (argc==5 && std::string(argv[1])=="--element")
    {
        Fixture f; f.root=argv[2]; f.role=argv[3]; f.topic=argv[4];
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(100); return runtime.Run(f);
    }
    assert(argc==3); alarm(100);
    const std::string scenario=argv[2];
    const auto baseline=Fds();
    {
        kcf::Process child;
        assert(child.ForceStop()==-ESRCH);
        assert(child.Start("/bin/sleep",{"10"})==0);
        assert(child.ForceStop()==0);
        assert(child.Wait()==0);
        assert(child.GetExitInfo().signaled && child.GetExitInfo().signal_number==SIGKILL);
        assert(child.ForceStop()==-ESRCH && child.GetSupervisionFd()==-1);
    }
    const fs::path root=fs::path("/tmp")/("kcf-reset-"+std::to_string(getpid()));
    fs::create_directory(root);
    const std::string topic="/kcf/reset/"+std::to_string(getpid());
    const auto executable = root/"element";
    fs::create_symlink(fs::absolute(argv[0]), executable);
    bringup::Bringup app;
    app.RequestReset(); // INITIALIZING is rejected
    std::vector<bringup::ElementSpec> specs{
        {"safe",argv[1],{},2000,1000,300},
        {"A",executable.string(),{"--element",root.string(),"A",topic},1500,1000,300},
        {"B",executable.string(),{"--element",root.string(),"B",topic},1000,300,300}};
    assert(app.Setup(specs)==0);
    std::uint64_t ticks=0;assert(kcf::detail::ReadProcessIdentity(getpid(),ticks));
    const auto status_path="/dev/shm/kcf%2Fsystem%2Fstatus%2Fstate_"+kcf::detail::SystemStatusScope(getpid(),ticks);
    const auto initial=app.GetElementStatuses();
    assert(initial.size()==3);
    std::thread controller([&]
    {
        kcf::SystemStatusSubscriber status;
        assert(status.OpenForApplication(getpid(),ticks)==0);
        auto snapshot=[&] { kcf::SystemStatus v{}; assert(status.ReadCurrent(v)==0); return v; };
        auto state=[&](State s) { Until([&]{return snapshot().state==s;}); };
        auto ids=[&] {
            std::vector<pid_t> result;
            std::ifstream f("/proc/self/task/"+std::to_string(getpid())+"/children");
            pid_t pid;while(f>>pid)result.push_back(pid);return result;
        };
        auto bpid=[&] { return static_cast<pid_t>(std::stoi(Read(root/"B.pid"))); };
        auto absent=[&](const auto& old) { for(auto pid:old)assert(!fs::exists("/proc/"+std::to_string(pid))); };
        struct stat inode{}; assert(stat(status_path.c_str(),&inode)==0);
        const auto stable=Fds();
        app.RequestReset(); // RUNNING: no deferred restart
        std::this_thread::sleep_for(250ms);
        assert(snapshot().state==State::RUNNING && ids().size()==3);
        const int cycles=scenario=="repeat"?10:1;
        for (int cycle=0;cycle<cycles;++cycle)
        {
            auto old=ids(); assert(old.size()==3);
            const auto pid=bpid();
            if (scenario=="repeat" && cycle==2) Write(root/"stall","yes");
            else assert(kill(pid, scenario=="repeat" && cycle==1 ? SIGSTOP : cycle==0 ? SIGSEGV : SIGKILL)==0);
            state(State::ERROR);
            const auto origin=snapshot(); assert(origin.error_valid && origin.failed_pid==pid);
            if(scenario=="repeat" && cycle==1)assert(origin.failure_kind==kcf::ElementFailureKind::STATUS_TIMEOUT);
            if(scenario=="repeat" && cycle==2)assert(origin.failure_kind==kcf::ElementFailureKind::HEARTBEAT_STALL);
            std::this_thread::sleep_for(150ms);assert(snapshot().state==State::ERROR);
            Write(root/"stall","no");
            Write(root/"B.mode", scenario=="failure"?"fail":scenario=="incomplete"?"incomplete":"delay");
            if(scenario=="execfailure") fs::remove(executable);
            app.RequestReset();
            state(State::RESETTING);
            assert(snapshot().error_valid && snapshot().failed_pid==pid);
            for(int i=0;i<20;++i) app.RequestReset(); // duplicate requests rejected
            if(scenario=="shutdown" || scenario=="shutdown-new")
            {
                if(scenario=="shutdown-new") Until([&]{return bpid()!=pid;});
                assert(kill(getpid(),SIGTERM)==0);
                state(State::SHUTTING_DOWN);
                app.RequestReset();
                status.Close();return;
            }
            if(scenario=="execfailure")
            {
                state(State::ERROR);absent(old);assert(ids().empty());
                assert(snapshot().error_valid && snapshot().failed_pid==pid);
                std::this_thread::sleep_for(300ms);assert(ids().empty());
                fs::create_symlink(fs::absolute(argv[0]),executable);
                app.RequestReset();state(State::RESETTING);
            }
            if(scenario=="failure")
            {
                state(State::ERROR);absent(old);assert(ids().empty());
                assert(snapshot().error_valid && snapshot().failed_pid==pid);
                std::this_thread::sleep_for(300ms);assert(ids().empty()); // no retry
                Write(root/"B.mode","hang");app.RequestReset();state(State::RESETTING);
                state(State::ERROR);assert(ids().empty()); // startup timeout + forced cleanup
                assert(snapshot().failed_pid==pid);
                Write(root/"B.mode","delay");app.RequestReset();state(State::RESETTING);
            }
            if(scenario=="incomplete")
            {
                Until([&]{return !Read(root/"incomplete.ready").empty();});
                const auto initializing=static_cast<pid_t>(std::stoi(Read(root/"incomplete.ready")));
                assert(initializing!=pid && kill(initializing,SIGKILL)==0);
                state(State::ERROR);absent(old);assert(ids().empty());
                assert(!fs::exists("/proc/"+std::to_string(initializing)));
                assert(snapshot().error_valid && snapshot().failed_pid==pid);
                Write(root/"B.mode","delay");
                app.RequestReset();state(State::RESETTING);
            }
            Until([&]{
                auto v=snapshot();
                if(v.state==State::RESETTING) assert(v.error_valid && v.failed_pid==pid);
                return v.state==State::RUNNING;
            });
            absent(old);assert(ids().size()==3 && bpid()!=pid);
            assert(!snapshot().error_valid && snapshot().failure_kind==kcf::ElementFailureKind::NONE);
            assert(Fds()==stable);
            struct stat current{};assert(stat(status_path.c_str(),&current)==0 && current.st_ino==inode.st_ino);
            kcf::SharedChannel<pid_t> peer;assert(peer.Open(topic)==0);
            Until([&]{pid_t value=0;std::uint32_t seq=0;return peer.ReadLatestSnapshot(value,seq)==0 && value==bpid();});
            Until([&]{return Read(root/"A.observed")==std::to_string(bpid());});
            assert(peer.Close()==0);
            kcf::SharedParameter<pid_t> settings;
            assert(settings.Open(topic+"/parameter")==0);
            pid_t owner=0;std::uint64_t version=0;
            assert(settings.Get(owner,&version)==0 && owner==bpid());
            assert(settings.Close()==0);
            std::this_thread::sleep_for(200ms);assert(snapshot().state==State::RUNNING);
            std::cout<<"[ResetTest] cycle="<<cycle+1<<" PASS"<<std::endl;
        }
        assert(kill(getpid(),SIGTERM)==0);
        status.Close();
    });
    const int result=app.Run();controller.join();
    assert(result==(scenario.find("shutdown")==0?1:0));
    assert(app.GetApplicationState()==State::SHUTTING_DOWN);
    assert(app.GetApplicationError().valid==(scenario.find("shutdown")==0));
    for(const auto& e:app.GetElementStatuses())assert(!e.running);
    int status;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    assert(Fds()==baseline);
    assert(!fs::exists(status_path));
    // Shutdown during Reset can leave a dead owner object, intentionally
    // retained for the next controlled generation. Test owns only this name.
    kcf::SharedChannel<pid_t> cleanup;assert(cleanup.Create(topic)==0);cleanup.Close();cleanup.Unlink();
    kcf::SharedParameter<pid_t> cleanup_parameter;
    assert(cleanup_parameter.Create(topic+"/parameter",0)==0);cleanup_parameter.Close();cleanup_parameter.Unlink();
    fs::remove_all(root);
    std::cout<<"Reset "<<scenario<<" PASS"<<std::endl;
}
