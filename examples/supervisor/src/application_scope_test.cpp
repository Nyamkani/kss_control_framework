#include "bringup/bringup.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/system/system_status_channel.hpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <sys/prctl.h>
using namespace std::chrono_literals;
namespace fs=std::filesystem;
using State=kcf::ApplicationState;
std::atomic<bool> reset{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void ResetSignal(int){reset.store(true);}
void Write(const fs::path& path,const std::string& text){
    const auto tmp=path.string()+".tmp";{std::ofstream out(tmp);out<<text;}fs::rename(tmp,path);
}
std::string Read(const fs::path& path){std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){
    assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
struct Child:kcf::ProcessElement{
    fs::path root;std::string label;kcf::SystemStatusSubscriber subscriber;
    Child(fs::path r,std::string l):root(std::move(r)),label(std::move(l)){}
    int Setup()override{
        Write(root/(label+".scope"),kcf::detail::InheritedSystemStatusScope());
        Write(root/(label+".pid"),std::to_string(getpid()));
        return subscriber.Open([&](const kcf::SystemStatus& value){
            Write(root/(label+".state"),std::to_string(static_cast<int>(value.state)));
        });
    }
    int Loop()override{return label=="worker" && fs::exists(root/"fail")?-EIO:0;}
    void Shutdown()override{subscriber.Close();}
};
kcf::SupervisorInfo Discover(pid_t pid){kcf::IntrospectionClient client;std::vector<kcf::SupervisorInfo> all;
    assert(client.ListSupervisors(all)==0);for(const auto& s:all)if(s.pid==pid)return s;return {};}
State Status(kcf::SystemStatusSubscriber& reader){kcf::SystemStatus value;assert(reader.ReadCurrent(value)==0);return value.state;}
pid_t Start(const char* exe,const std::string& name,const fs::path& root){
    fs::create_directories(root);auto pid=fork();assert(pid>=0);
    if(!pid){execl(exe,exe,"--application",name.c_str(),root.c_str(),nullptr);_exit(127);}return pid;
}
void Stop(pid_t pid,int sig=SIGTERM){assert(kill(pid,sig)==0);int code;assert(waitpid(pid,&code,0)==pid);
    assert(sig==SIGKILL?WIFSIGNALED(code):(WIFEXITED(code)&&WEXITSTATUS(code)==0));}
int main(int argc,char** argv){
    assert(prctl(PR_SET_PDEATHSIG,SIGTERM)==0);
    if(argc==4 && std::string(argv[1])=="--child"){
        Child child(argv[2],argv[3]);kcf::ProcessRuntime runtime;runtime.SetLoopFrequency(100);return runtime.Run(child)?1:0;
    }
    if(argc==4 && std::string(argv[1])=="--application"){
        signal(SIGUSR1,ResetSignal);bringup::Bringup app;
        assert(app.Setup(argv[2],{{"observer",argv[0],{"--child",argv[3],"observer"}},
                                 {"worker",argv[0],{"--child",argv[3],"worker"}}})==0);
        std::atomic<bool> done{false};std::thread controller([&]{while(!done){if(reset.exchange(false))app.RequestReset();std::this_thread::sleep_for(5ms);}});
        int result=app.Run();done=true;controller.join();return result;
    }
    alarm(90);
    const auto root=fs::temp_directory_path()/("kcf_scope_"+std::to_string(getpid()));
    // A nested/new Supervisor must replace an inherited parent scope for its children.
    assert(setenv(kcf::detail::SYSTEM_STATUS_SCOPE_ENV,"123_456",1)==0);
    for(bool same:{false,true}){
        auto aroot=root/(same?"same_a":"a"),broot=root/(same?"same_b":"b");
        pid_t a=Start(argv[0],same?"same_app":"app_a",aroot),b=Start(argv[0],same?"same_app":"app_b",broot);
        kcf::SupervisorInfo ai,bi;
        Until([&]{ai=Discover(a);bi=Discover(b);return ai.pid && bi.pid && ai.application_state==State::RUNNING && bi.application_state==State::RUNNING;});
        const auto ascope=kcf::detail::SystemStatusScope(ai.pid,ai.process_start_ticks);
        const auto bscope=kcf::detail::SystemStatusScope(bi.pid,bi.process_start_ticks);assert(ascope!=bscope);
        kcf::SystemStatusSubscriber ar,br;
        assert(ar.OpenForApplication(a,ai.process_start_ticks)==0 && br.OpenForApplication(b,bi.process_start_ticks)==0);
        const auto children=[&](const fs::path& path,State state,const std::string& scope){return Read(path/"observer.state")==std::to_string(static_cast<int>(state)) && Read(path/"observer.scope")==scope;};
        Until([&]{return children(aroot,State::RUNNING,ascope) && children(broot,State::RUNNING,bscope);});
        // ERROR/SAFE isolation in both directions, including new child generations.
        for(int side=0;side<2;++side){auto& target=side?broot:aroot;auto& other=side?aroot:broot;
            auto& reader=side?br:ar;auto& peer=side?ar:br;const auto pid=side?b:a;
            const auto scope=side?bscope:ascope,peer_scope=side?ascope:bscope;
            const auto old_pid=Read(target/"observer.pid");Write(target/"fail","1");
            Until([&]{return Status(reader)==State::ERROR && children(target,State::ERROR,scope);});
            assert(Status(peer)==State::RUNNING && children(other,State::RUNNING,peer_scope));
            fs::remove(target/"fail");assert(kill(pid,SIGUSR1)==0);
            Until([&]{return Status(reader)==State::RUNNING && Read(target/"observer.pid")!=old_pid && children(target,State::RUNNING,scope);});
            assert(Status(peer)==State::RUNNING && children(other,State::RUNNING,peer_scope));
            auto current=Discover(pid);assert(current.process_start_ticks==(side?bi:ai).process_start_ticks);
        }
        ar.Close();
        if(same){
            Stop(a,SIGKILL);
            // A stale object is left behind. A new same-name instance must not collide.
            pid_t replacement=Start(argv[0],"same_app",root/"replacement");kcf::SupervisorInfo info;
            Until([&]{info=Discover(replacement);return info.pid && info.application_state==State::RUNNING;});
            assert(info.pid!=ai.pid || info.process_start_ticks!=ai.process_start_ticks);
            kcf::SystemStatusSubscriber fresh;assert(fresh.OpenForApplication(info.pid,info.process_start_ticks)==0);
            assert(Status(fresh)==State::RUNNING);fresh.Close();Stop(replacement);
            // Reclaim only this test's known dead-owner object using existing recovery,
            // then unlink it as its new owner. No application performs this cleanup.
            std::string name;assert(kcf::detail::SystemStatusName(ascope,name)==0);
            kcf::SharedParameter<kcf::SystemStatus> stale;assert(stale.Create(name,{})==0);assert(stale.Unlink()==0);stale.Close();
        }else Stop(a);
        kcf::SystemStatusSubscriber gone;assert(gone.OpenForApplication(ai.pid,ai.process_start_ticks)==-ENOENT);
        assert(Status(br)==State::RUNNING && children(broot,State::RUNNING,bscope));br.Close();Stop(b);
        assert(gone.OpenForApplication(bi.pid,bi.process_start_ticks)==-ENOENT);
        std::cout<<(same?"same-name/crash":"different-name")<<" isolation/reset/shutdown PASS\n";
    }
    // No scope retains the legacy standalone contract; invalid scopes fail closed.
    assert(unsetenv(kcf::detail::SYSTEM_STATUS_SCOPE_ENV)==0);
    kcf::SystemStatusPublisher writer;assert(writer.Create({})==0);
    kcf::SystemStatusSubscriber reader;assert(reader.Open()==0);reader.Close();writer.Close();writer.Unlink();
    setenv(kcf::detail::SYSTEM_STATUS_SCOPE_ENV,"../invalid",1);assert(reader.Open()==-EINVAL);unsetenv(kcf::detail::SYSTEM_STATUS_SCOPE_ENV);
    fs::remove_all(root);std::cout<<"Application-scoped SystemStatus PASS\n";
}
