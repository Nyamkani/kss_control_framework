// Supervisor contract only. Runtime Setup/exception/Shutdown coverage remains in
// kcf_runtime_lifecycle_test; children reuse kcf_supervisor_runtime_test.
#include "bringup/bringup.hpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/syscall.h>

// Test executable interposition only: production Process/Bringup have no test hooks.
static pid_t fault_pid=-1;
static int fault_mode=0, force_attempts=0;
extern "C" int kill(pid_t pid,int signal) noexcept {
    if(pid==fault_pid && (signal==SIGTERM || (signal==SIGKILL && fault_mode==2))) {
        if(signal==SIGKILL) ++force_attempts;
        errno=EPERM;return -1;
    }
    return static_cast<int>(syscall(SYS_kill,pid,signal));
}
extern "C" pid_t waitpid(pid_t pid,int* status,int options) {
    if(pid==fault_pid && fault_mode==3) {errno=EIO;return -1;}
    return static_cast<pid_t>(syscall(SYS_wait4,pid,status,options,nullptr));
}
using namespace std::chrono_literals;
using State = kcf::ApplicationState;
namespace fs = std::filesystem;

static std::size_t Fds() {
    return std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator{});
}
template<class F> static void Until(F predicate) {
    const auto end=std::chrono::steady_clock::now()+8s;
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<end);
        std::this_thread::sleep_for(5ms);
    }
}
static void Cleanup(const bringup::Bringup& app,const std::string& path,std::size_t baseline) {
    for(const auto& e:app.GetElementStatuses()) {
        assert(!e.running && e.pid>0);
        assert(!fs::exists("/proc/"+std::to_string(e.pid)));
        assert(e.exit_info.available);
    }
    int status=0;errno=0;
    assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    assert(!fs::exists(path));
    assert(Fds()==baseline);
}
static std::vector<bringup::ElementSpec> Specs(const std::string& executable,
                                              bool fail,unsigned shutdown_delay) {
    return {{"healthy",executable,{"--element-name","healthy"},3000,2000,1000},
            {"target",executable,{"--element-name","target","--setup-delay-ms","300",
                "--setup-result",fail?"7":"0","--shutdown-delay-ms",std::to_string(shutdown_delay)},
                3000,2000,1000}};
}
// Exec the existing Runtime fixture with a per-generation Setup result.
// No Runtime callback implementation is duplicated here.
static void Mode(const std::string& path,int result) {
    std::ofstream out(path);out<<result;out.close();assert(out);
}
static void InitialFailure(const std::string& self,const std::string& executable,
                           std::uint64_t ticks,const std::string& path,unsigned delay,
                           bool reset,bool interrupt_reset=false) {
    const auto baseline=Fds();
    const std::string mode="/tmp/kcf-failure-mode-"+std::to_string(getpid());
    Mode(mode,7);
    bringup::Bringup app;
    auto specs=Specs(executable,true,delay);
    specs[1].executable=self;
    specs[1].arguments={"--fixture",executable,mode,std::to_string(delay)};
    assert(app.Setup("failure-contract",specs)==0);
    assert(app.GetApplicationState()==State::ERROR && app.GetApplicationError().valid);
    const auto initial=app.GetElementStatuses();
    assert(initial.size()==2 && initial[0].running && !initial[0].failure_detected);
    assert(initial[1].failure_detected);
    const auto first=app.GetErrorHistory().front();
    assert(app.GetErrorHistory().size()==1 && first.generation==1 && first.sequence==1);
    assert(first.error.element_name=="target");
    if(first.error.failure_kind==kcf::ElementFailureKind::PROCESS_EXIT)
        assert(first.error.exit_info.exit_code==7 &&
               (first.runtime_error_observed || first.error.runtime_error==0));
    else assert(first.runtime_error_observed && first.error.runtime_error==7);
    std::thread controller([&]{
        kcf::SystemStatusSubscriber status;
        assert(status.OpenForApplication(getpid(),ticks)==0);
        auto snapshot=[&]{kcf::SystemStatus value{};assert(status.ReadCurrent(value)==0);return value;};
        auto state=[&](State expected){Until([&]{return snapshot().state==expected;});};
        state(State::ERROR);
        std::this_thread::sleep_for(250ms);
        assert(snapshot().state==State::ERROR && kill(initial[0].pid,0)==0);
        if(reset) {
            app.RequestReset();state(State::RESETTING);
            if(interrupt_reset) {
                app.RequestShutdown();app.RequestReset();
            } else {
                state(State::ERROR); // Same Setup error in generation 2.
                assert(snapshot().failed_pid==first.error.pid); // Original cause retained.
                std::this_thread::sleep_for(250ms);
                assert(snapshot().state==State::ERROR); // No automatic retry.
                Mode(mode,0);
                app.RequestReset();state(State::RESETTING);state(State::RUNNING);
                assert(!snapshot().error_valid);
                app.RequestShutdown();
            }
        } else app.RequestShutdown();
        app.RequestShutdown(); // Idempotent pending request.
        state(State::STOPPED); // Retained SHM mapping observes final publish.
        assert(status.Close()==0);
    });
    const int result=app.Run();controller.join();
    assert(result==(reset && !interrupt_reset?0:1));
    assert(app.GetApplicationState()==State::STOPPED);
    assert(app.GetApplicationError().valid==(!reset || interrupt_reset));
    const auto& history=app.GetErrorHistory();
    assert(history.size()==(reset && !interrupt_reset?2u:1u));
    assert(history.front().error.pid==first.error.pid);
    assert(history.front().error.exit_info.available);
    if(history.size()==2) {
        assert(history[1].generation==2 && history[1].sequence>first.sequence);
        assert(history[1].monotonic_ns>=first.monotonic_ns);
    }
    app.Shutdown(); // Completed shutdown is idempotent, preserves history.
    Cleanup(app,path,baseline);fs::remove(mode);
    std::cout<<"Initial ERROR delay="<<delay<<" reset="<<reset
             <<" interrupt_reset="<<interrupt_reset<<" history="<<history.size()<<" PASS\n";
}
static void RunningExit(const std::string& executable,std::uint64_t ticks,
                        const std::string& path,bool unexpected) {
    const auto baseline=Fds();
    bringup::Bringup app;
    assert(app.Setup("failure-contract",Specs(executable,false,0))==0);
    assert(app.GetApplicationState()==State::RUNNING && !app.GetApplicationError().valid);
    const auto children=app.GetElementStatuses();
    kcf::SystemStatus error{};
    std::thread controller([&]{
        kcf::SystemStatusSubscriber status;
        assert(status.OpenForApplication(getpid(),ticks)==0);
        kcf::SystemStatus current{};assert(status.ReadCurrent(current)==0);
        assert(current.state==State::RUNNING);
        if(unexpected) {
            assert(kill(children[1].pid,SIGKILL)==0);
            Until([&]{assert(status.ReadCurrent(error)==0);return error.state==State::ERROR;});
            assert(error.error_valid && error.failed_pid==children[1].pid);
            assert(error.failure_kind==kcf::ElementFailureKind::PROCESS_EXIT);
            const auto first_pid=error.failed_pid;
            assert(kill(children[0].pid,0)==0);
            assert(kill(children[0].pid,SIGKILL)==0);
            // Give the regular monitor cycles time to observe the second exit.
            Until([&]{return !fs::exists("/proc/"+std::to_string(children[0].pid));});
            assert(status.ReadCurrent(error)==0 && error.failed_pid==first_pid);
            // ERROR latches, but does not automatically terminate the healthy sibling.
        }
        assert(status.Close()==0);
        assert(kill(getpid(),SIGTERM)==0);
    });
    const int result=app.Run();controller.join();
    assert(result==(unexpected?1:0));
    assert(app.GetApplicationState()==State::STOPPED);
    assert(app.GetApplicationError().valid==unexpected);
    const auto statuses=app.GetElementStatuses();
    if(unexpected) {
        const auto& origin=app.GetApplicationError();
        assert(origin.element_name=="target" && origin.pid==children[1].pid);
        assert(origin.failure_kind==kcf::ElementFailureKind::PROCESS_EXIT);
        assert(origin.exit_info.signaled && origin.exit_info.signal_number==SIGKILL);
        const auto& history=app.GetErrorHistory();
        assert(history.size()==2 && history[0].error.pid==children[1].pid &&
               history[1].error.pid==children[0].pid);
        assert(history[0].sequence<history[1].sequence);
        assert(!history[0].runtime_error_observed && history[0].error.runtime_error==0);
    } else {
        assert(app.GetErrorHistory().empty());
        for(const auto& e:statuses)
            assert(!e.failure_detected && e.exit_info.exited_normally && e.exit_info.exit_code==0);
    }
    Cleanup(app,path,baseline);
    std::cout<<(unexpected?"Unexpected SIGKILL: RUNNING->ERROR->SHUTTING_DOWN->STOPPED":
                            "Requested SIGTERM: RUNNING->SHUTTING_DOWN->STOPPED")
             <<" return="<<result<<" cleanup PASS\n";
}
static void StartupBoundary(const std::string& executable,std::uint64_t ticks,
                            const std::string& path,bool cancel) {
    const auto baseline=Fds();
    bringup::Bringup app;
    auto specs=Specs(executable,false,0);
    specs[1].arguments={"--setup-delay-ms","600"};
    specs[1].startup_timeout_ms=cancel?3000:100;
    if(cancel) {
        std::thread controller([&]{
            kcf::SystemStatusSubscriber reader;
            Until([&]{return reader.OpenForApplication(getpid(),ticks)==0;});
            kcf::SystemStatus value{};assert(reader.ReadCurrent(value)==0);
            assert(value.state==State::INITIALIZING);
            app.RequestShutdown();
            Until([&]{assert(reader.ReadCurrent(value)==0);return value.state==State::STOPPED;});
            assert(reader.Close()==0);
        });
        assert(app.Setup("startup-cancel",specs)==-ECANCELED);
        controller.join();
        assert(app.GetErrorHistory().empty());
    } else {
        assert(app.Setup("startup-timeout",specs)==0);
        assert(app.GetApplicationState()==State::ERROR);
        assert(app.GetApplicationError().failure_kind==kcf::ElementFailureKind::STATUS_TIMEOUT);
        assert(!app.GetErrorHistory().front().runtime_error_observed);
        assert(app.GetElementStatuses()[0].running);
        app.RequestShutdown();assert(app.Run()==1);
    }
    assert(app.GetApplicationState()==State::STOPPED);
    // A cancellation before the second fork need not have a second child.
    for(const auto& e:app.GetElementStatuses())
        if(e.pid>0) assert(!e.running && !fs::exists("/proc/"+std::to_string(e.pid)));
    int status=0;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    assert(!fs::exists(path) && Fds()==baseline);
    std::cout<<"Startup "<<(cancel?"explicit cancellation":"barrier timeout")<<" PASS\n";
}
static void TestSignalHandler(int) {}
static void ShutdownFault(const std::string& executable,const std::string& path,int mode) {
    const auto baseline=Fds();
    struct sigaction saved_int{},saved_term{},custom{};
    custom.sa_handler=TestSignalHandler;sigemptyset(&custom.sa_mask);
    assert(sigaction(SIGINT,&custom,&saved_int)==0);
    assert(sigaction(SIGTERM,&custom,&saved_term)==0);
    bringup::Bringup app;
    auto specs=Specs(executable,false,0);
    for(auto& spec:specs)spec.shutdown_timeout_ms=100;
    assert(app.Setup("shutdown-fault",specs)==0);
    const auto children=app.GetElementStatuses();
    // Keep the first child stopped so a denied kill cannot be masked by its
    // own Supervisor-loss timeout. The second child must still be cleaned up.
    assert(kill(children[0].pid,SIGSTOP)==0);
    fault_pid=children[0].pid;fault_mode=mode;force_attempts=0;
    const auto begin=std::chrono::steady_clock::now();
    app.RequestShutdown();assert(app.Run()==1);
    assert(std::chrono::steady_clock::now()-begin<4s);
    assert(!app.GetApplicationError().valid && app.GetErrorHistory().empty());
    assert(!fs::exists(path));
    struct sigaction current{};
    assert(sigaction(SIGINT,nullptr,&current)==0 && current.sa_handler==TestSignalHandler);
    assert(sigaction(SIGTERM,nullptr,&current)==0 && current.sa_handler==TestSignalHandler);
    auto statuses=app.GetElementStatuses();
    assert(!statuses[1].running && statuses[1].exit_info.available);
    assert(!fs::exists("/proc/"+std::to_string(children[1].pid)));
    if(mode==1) {
        assert(app.GetApplicationState()==State::STOPPED);
        Cleanup(app,path,baseline);
        app.Shutdown();Cleanup(app,path,baseline);
    } else {
        assert(app.GetApplicationState()==State::SHUTTING_DOWN);
        assert(statuses[0].pid==children[0].pid && !statuses[0].exit_info.available);
        assert(fs::exists("/proc/"+std::to_string(children[0].pid)));
        assert(Fds()==baseline+1); // Retained ownership includes its supervision FD.
        assert(app.Run()==-EINVAL); // No implicit restart of the monitor/generation.
        if(mode==2) {
            assert(force_attempts==1);
            app.Shutdown(); // Explicit retry only, still bounded.
            assert(force_attempts==2 && app.GetApplicationState()==State::SHUTTING_DOWN);
        }
    }
    fault_pid=-1;fault_mode=0;
    app.Shutdown(); // Explicit cleanup after the OS fault is removed.
    assert(app.GetApplicationState()==State::STOPPED);
    Cleanup(app,path,baseline);
    app.Shutdown();Cleanup(app,path,baseline);
    assert(sigaction(SIGINT,&saved_int,nullptr)==0);
    assert(sigaction(SIGTERM,&saved_term,nullptr)==0);
    std::cout<<"Shutdown fault mode="<<mode<<" ownership/FD/SHM/signal/idempotence PASS\n";
}
int main(int argc,char** argv) {
    if(argc==5 && std::string(argv[1])=="--fixture") {
        int code=0;std::ifstream in(argv[3]);in>>code;assert(in);
        const auto value=std::to_string(code);
        execl(argv[2],argv[2],"--element-name","target","--setup-delay-ms","300",
              "--setup-result",value.c_str(),"--shutdown-delay-ms",argv[4],nullptr);
        _exit(127);
    }
    assert(argc==2);alarm(60);
    std::uint64_t ticks=0;assert(kcf::detail::ReadProcessIdentity(getpid(),ticks));
    const std::string path="/dev/shm/kcf%2Fsystem%2Fstatus%2Fstate_"+
                           kcf::detail::SystemStatusScope(getpid(),ticks);
    { bringup::Bringup invalid;assert(invalid.Setup(std::vector<bringup::ElementSpec>{})==-EINVAL);
      assert(invalid.GetErrorHistory().empty() && !fs::exists(path)); }
    InitialFailure(argv[0],argv[1],ticks,path,300,false);
    InitialFailure(argv[0],argv[1],ticks,path,0,false);
    InitialFailure(argv[0],argv[1],ticks,path,300,true);
    InitialFailure(argv[0],argv[1],ticks,path,300,true,true);
    RunningExit(argv[1],ticks,path,true);
    RunningExit(argv[1],ticks,path,false);
    StartupBoundary(argv[1],ticks,path,false);
    StartupBoundary(argv[1],ticks,path,true);
    ShutdownFault(argv[1],path,1);
    ShutdownFault(argv[1],path,2);
    ShutdownFault(argv[1],path,3);
    std::cout<<"Supervisor failure contract PASS\n";
}
