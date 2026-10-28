#include "kcf/process/process_runtime.hpp"
#include "kcf/process/process_element.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/network/local_metadata.hpp"
#include "../../network/src/parameter_demo_type.hpp"
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
using namespace std::chrono_literals;
class Child : public kcf::ProcessElement {
    std::string name_;kcf::Subscriber<Value> input_;
public:explicit Child(std::string name):name_(std::move(name)){}
    int Setup() override{return input_.Create(name_);}
    int Loop() override{return 0;}
    void Shutdown() override{input_.Close();}
};
static pid_t Launch(std::vector<std::string> args){pid_t pid=fork();assert(pid>=0);if(!pid){std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);execv(argv[0],argv.data());_exit(127);}return pid;}
static void Wait(pid_t pid,bool ok){int status=0;for(int i=0;i<500;++i){if(waitpid(pid,&status,WNOHANG)==pid){assert(WIFEXITED(status)&&(ok?WEXITSTATUS(status)==0:WEXITSTATUS(status)!=0));return;}std::this_thread::sleep_for(10ms);}kill(pid,SIGKILL);waitpid(pid,nullptr,0);assert(false);}
int main(int argc,char** argv){
    if(argc>1&&std::string(argv[1])=="--child"){
        // Any leaked Supervisor option makes the child fail startup.
        if(argc!=4||std::string(argv[3])!="sentinel")return 99;
        Child child(argv[2]);kcf::ProcessRuntime runtime;runtime.SetLoopFrequency(50);return runtime.Run(child);
    }
    assert(argc==2);alarm(60);const std::string launcher=argv[1];
    std::vector<std::vector<std::string>> invalid{{"--element-name"},{"--element-name",""},{"--element-name","Motor","--element-name","IMU"},{"--element-name","--health-timeout-ms","100"},{"--element-name","same","--next",argv[0],"--element-name","same"}};
    for(const auto& tail:invalid){std::vector<std::string> args{launcher,argv[0]};args.insert(args.end(),tail.begin(),tail.end());Wait(Launch(args),false);}
    const std::string topic="/graph_name_test_"+std::to_string(getpid());kcf::Publisher<Value> publisher;assert(!publisher.Create(topic));
    kcf::IntrospectionClient client;std::vector<int> prior;
    for(int run=0;run<3;++run){const bool named=run!=0;std::vector<std::string> names=named?std::vector<std::string>{"Motor","IMU","EKF"}:std::vector<std::string>{"element1","element2"};
        std::vector<std::string> args{launcher,"--application-name","NameProbe"};for(const auto& name:names){if(args.size()>3)args.push_back("--next");args.insert(args.end(),{argv[0],"--child",topic,"sentinel","--startup-timeout-ms","3000","--health-timeout-ms","2000","--shutdown-timeout-ms","2000"});if(named)args.insert(args.end(),{"--element-name",name});}
        auto pid=Launch(args);kcf::SupervisorInfo info{};bool ready=false;
        for(int attempt=0;attempt<500&&!ready;++attempt){std::vector<kcf::SupervisorInfo> all;assert(!client.ListSupervisors(all));for(const auto& s:all)if(s.pid==pid&&s.application_state==kcf::ApplicationState::RUNNING){info=s;ready=true;}if(!ready)std::this_thread::sleep_for(10ms);}assert(ready);
        assert(info.element_count==names.size()&&std::string(info.application_name)=="NameProbe");
        kcf::network::MetadataConfig config;config.host.bytes[0]=1;config.boot_id.bytes[0]=2;
        std::vector<int> current;
        for(std::size_t i=0;i<names.size();++i){assert(info.elements[i].name==names[i]);current.push_back(info.elements[i].pid);config.runtime_pids.push_back(info.elements[i].pid);}
        assert(current!=prior);prior=current;
        std::vector<kcf::network::LocalEndpoint> endpoints;assert(!kcf::network::CollectLocalMetadata(config,endpoints));assert(endpoints.size()==names.size());
        for(const auto& e:endpoints){assert(e.metadata.application_name=="NameProbe"&&e.metadata.application.pid==pid);bool found=false;for(std::size_t i=0;i<names.size();++i)if(e.metadata.identity.runtime.pid==info.elements[i].pid){assert(e.metadata.element_name==names[i]&&e.metadata.identity.runtime.process_start_ticks==info.elements[i].process_start_ticks);found=true;}assert(found);}
        kill(pid,SIGTERM);Wait(pid,true);
    }
    publisher.Close();publisher.Unlink();std::cout<<"Default/explicit names, argv consumption, invalid names, Supervisor/Metadata identity and restart PASS\n";
}
