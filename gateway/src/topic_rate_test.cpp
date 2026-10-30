// Isolated virtual Hosts: test-only Topic SHM namespace partition, as in topic_data_test.
#include "gateway_support.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/ipc/command_freshness.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
using namespace std::chrono_literals;
namespace n=kcf::network;
using Clock=std::chrono::steady_clock;
static std::string prefix;
static bool fresh=false,initial_frozen=false;
static int publish_hz=100;
static std::mutex timing_mutex;
static std::vector<double> lateness;
namespace kcf::network {
void TopicSchedulerTestSample(Clock::time_point deadline,Clock::time_point actual){
    std::lock_guard<std::mutex> lock(timing_mutex);
    if(lateness.size()<10000)lateness.push_back(std::chrono::duration<double,std::micro>(actual-deadline).count());
}
}
static std::string Map(const char* name){return !prefix.empty()&&(std::strncmp(name,"/kcf_remote_topic_",18)==0||std::strncmp(name,"/kcf_network_proxy_",19)==0)?prefix+(name+1):std::string(name);}
extern "C" int shm_open(const char* n,int f,mode_t m){static auto real=reinterpret_cast<int(*)(const char*,int,mode_t)>(dlsym(RTLD_NEXT,"shm_open"));return real(Map(n).c_str(),f,m);}
extern "C" int shm_unlink(const char* n){static auto real=reinterpret_cast<int(*)(const char*)>(dlsym(RTLD_NEXT,"shm_unlink"));return real(Map(n).c_str());}
struct Sample {std::uint64_t sequence,stamp;};
namespace kcf {template<>struct TypeDescriptorTraits<Sample>{static constexpr bool defined=true;static TypeDescriptor Get(){auto d=MakeTypeDescriptor<Sample>("gateway.command.v1");d.field_count=2;d.fields[0]=MakeField<Sample,std::uint64_t>("sequence",offsetof(Sample,sequence));d.fields[1]=MakeField<Sample,std::uint64_t>("stamp",offsetof(Sample,stamp));return d;}};}
static std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
static gateway::Options Parse(std::vector<std::string> args){std::vector<char*> v;for(auto& a:args)v.push_back(a.data());return gateway::Parse(v.size(),v.data());}
struct Summary{double min{},mean{},p50{},p95{},p99{},max{};};
static Summary Summarize(std::vector<double> v){Summary s;if(v.empty())return s;std::sort(v.begin(),v.end());s.min=v.front();for(auto x:v)s.mean+=x;s.mean/=v.size();s.p50=v[(v.size()-1)*50/100];s.p95=v[(v.size()-1)*95/100];s.p99=v[(v.size()-1)*99/100];s.max=v.back();return s;}
struct Report {int watchdog_state{};double command_output{};int code{};std::uint64_t session{},published{},local{},callbacks{},sent{},received{},skipped{},missed{},dropped{},duplicate{},reverse{},last{},stamp{};std::size_t proxies{},routes{};double cpu_seconds{};Summary age,interval,send_lateness;};
static const std::string topic="/kcf_remote_topic_rate_cmd";
static void Child(int fd,int host,bool override_rate){
    lateness.reserve(10000);alarm(90);prefix="/rate_h"+std::to_string(host)+"_";
    kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING,0);
    auto options=host==1?Parse({"x","--topic-allow-prefix","/kcf_remote_topic_rate_cmd"}):Parse({"x","--remote-topic",topic});
    if(host==1&&override_rate)options=Parse({"x","--topic-allow-prefix",topic,"--topic-rate",topic+":100"});
    if(fresh)options.fresh_start_topics=Parse({"x","--topic-fresh-start",topic}).fresh_start_topics;
    n::NetworkInterfaceSelection nic{"lo","127.0.0."+std::to_string(host),1,false};
    auto cfg=gateway::Config(options,nic);cfg.discovery.host.bytes[0]=host;cfg.discovery.boot_id.bytes[0]=42;
    cfg.discovery.port=48651;cfg.discovery.metadata_port=49000+host;cfg.topics.port=48652;
    cfg.discovery.announce_interval=50ms;cfg.discovery.peer_timeout=400ms;cfg.discovery.metadata_retry=40ms;cfg.metadata_interval=40ms;
    cfg.metadata.runtime_pids={getpid()};
    // Existing typed bootstrap only: production common Gateway has no schema CLI.
    if(host!=1)cfg.topics.local_types={{topic,kcf::TypeDescriptorTraits<Sample>::Get()}};
    kcf::Publisher<Sample> publisher;kcf::Subscriber<Sample> subscriber,local;
    std::atomic<std::uint64_t> local_count{0};
    if(host==1){assert(!publisher.Create(topic));assert(!local.Create(topic,[&](const Sample&){++local_count;}));}
    n::ControlNode node;assert(!node.Start(cfg));
    std::mutex mutex;std::vector<double> ages,intervals;ages.reserve(10000);intervals.reserve(10000);
    void* blocked=MAP_FAILED;kcf::detail::ChannelLayout layout{};
    std::uint64_t count=0,last=0,stamp=0,last_time=0,duplicates=0,reverse=0,published=0;bool frozen=initial_frozen,subscribed=false;
    kcf::CommandFreshnessMonitor watchdog(100ms);bool watching=false;Sample command{};
    const Sample repeated{123,Now()};
    auto next=Clock::now();
    while(true){
        if(host==1&&!frozen&&Clock::now()>=next){Sample sample{published+1,Now()};assert(!publisher.Publish(sample));++published;const auto period=std::chrono::nanoseconds(1000000000/publish_hz);next+=period;if(next<Clock::now())next=Clock::now()+period;}
        if(watching){Sample next_command{};kcf::TopicReadInfo info;const int result=subscriber.ReadNext(next_command,info);if(watchdog.ObserveRead(result,info))command=next_command;}
        pollfd p{fd,POLLIN,0};int rc=poll(&p,1,1);assert(rc>=0);if(!rc)continue;
        char op;assert(recv(fd,&op,1,0)==1);Report report;
        if(op=='U'&&!subscribed){report.code=subscriber.Create(topic,[&](const Sample& s){const auto now=Now();std::lock_guard<std::mutex> lock(mutex);if(last){duplicates+=s.sequence==last;reverse+=s.sequence<last;}last=s.sequence;stamp=s.stamp;++count;if(ages.size()<10000)ages.push_back((now-s.stamp)/1000.0);if(last_time&&intervals.size()<10000)intervals.push_back((now-last_time)/1000.0);last_time=now;});subscribed=!report.code;}
        if(op=='W'&&!subscribed){watchdog.Reset();command={};report.code=subscriber.Create(topic,kcf::TopicStartPosition::NEXT);subscribed=!report.code;watching=subscribed;}
        if(op=='O'){report.code=subscriber.Close();subscribed=false;watching=false;watchdog.Reset();command={};}
        const auto validity=watchdog.GetSnapshot().state;report.watchdog_state=static_cast<int>(validity);
        report.command_output=validity==kcf::CommandFreshnessState::VALID?command.sequence:0;

        if(op=='N')node.Stop();
        if(op=='R')report.code=node.Start(cfg);
        if(op=='F')frozen=true;
        if(op=='V'){assert(!publisher.Publish(repeated));++published;}
        if(op=='L'){
            local.Close();int storage=shm_open(topic.c_str(),O_RDWR,0);assert(storage>=0);struct stat st{};assert(!fstat(storage,&st));
            blocked=mmap(nullptr,st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage,0);close(storage);assert(blocked!=MAP_FAILED);
            assert(kcf::detail::ComputeChannelLayout(sizeof(Sample),alignof(Sample),1,layout));
            for(unsigned i=0;i<3;++i)reinterpret_cast<kcf::detail::ChannelSlot<std::byte>*>(static_cast<char*>(blocked)+layout.slots+i*layout.stride)->users.store(UINT32_MAX);
        }
        if(op=='J'){
            for(unsigned i=0;i<3;++i)reinterpret_cast<kcf::detail::ChannelSlot<std::byte>*>(static_cast<char*>(blocked)+layout.slots+i*layout.stride)->users.store(0);
            munmap(blocked,layout.length);blocked=MAP_FAILED;assert(!local.Create(topic,[&](const Sample&){++local_count;}));
        }
        if(op=='X'){local.Close();publisher.Close();publisher.Unlink();assert(!publisher.Create(topic));assert(!local.Create(topic,[&](const Sample&){++local_count;}));assert(!publisher.Publish(repeated));++published;}

        if(op=='P'){frozen=false;next=Clock::now();}
        {std::lock_guard<std::mutex> lock(mutex);if(op=='M'){count=duplicates=reverse=last=last_time=0;ages.clear();intervals.clear();}
            report.callbacks=count;report.duplicate=duplicates;report.reverse=reverse;report.last=last;report.stamp=stamp;report.age=Summarize(ages);report.interval=Summarize(intervals);}
        {std::lock_guard<std::mutex> lock(timing_mutex);if(op=='M')lateness.clear();report.send_lateness=Summarize(lateness);}
        timespec cpu{};clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu);report.cpu_seconds=cpu.tv_sec+cpu.tv_nsec/1e9;
        report.published=published;report.local=local_count;report.session=node.GetSessionId();
        auto stats=node.GetTopicDiagnostics();report.proxies=stats.proxies.size();report.routes=stats.active_send_routes;
        for(const auto& r:stats.routes){report.sent+=r.packets_sent;report.received+=r.packets_received;report.skipped+=r.local_samples_skipped;report.missed+=r.missed_sequence;report.dropped+=r.dropped;}
        assert(send(fd,&report,sizeof(report),MSG_NOSIGNAL)==sizeof(report));if(op=='Q')break;
    }
    subscriber.Close();local.Close();node.Stop();if(host==1){publisher.Close();publisher.Unlink();}
    kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED,0);close(fd);
}
struct Host {pid_t pid;int fd;
    Host(int host,bool rate){int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));pid=fork();assert(pid>=0);if(!pid){close(pair[0]);Child(pair[1],host,rate);_exit(0);}close(pair[1]);fd=pair[0];}
    Report Ask(char op='S'){assert(send(fd,&op,1,MSG_NOSIGNAL)==1);pollfd p{fd,POLLIN,0};assert(poll(&p,1,10000)>0);Report r;assert(recv(fd,&r,sizeof(r),0)==sizeof(r));return r;}
    void End(){Ask('Q');int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));close(fd);pid=0;}
    ~Host(){if(pid>0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);close(fd);}}
};
template<class F>static void Until(F f){auto end=Clock::now()+10s;while(!f()){assert(Clock::now()<end);std::this_thread::sleep_for(20ms);}}
static void Distribution(const char* label,Summary s){std::cout<<label<<"_us min="<<s.min<<" mean="<<s.mean<<" p50="<<s.p50<<" p95="<<s.p95<<" p99="<<s.p99<<" max="<<s.max<<'\n';}
static std::size_t Entries(const char* path,const char* match=nullptr){auto* dir=opendir(path);assert(dir);std::size_t count=0;while(auto* entry=readdir(dir)){if(entry->d_name[0]=='.')continue;if(!match||std::strncmp(entry->d_name,match,std::strlen(match))==0)++count;}closedir(dir);return count;}
static void Run(bool rate){const auto fds=Entries("/proc/self/fd");Host pc(1,rate),robot(2,rate);
    Until([&]{return robot.Ask().proxies==1;});std::this_thread::sleep_for(300ms);assert(pc.Ask().sent==0); // no consumer => no payload
    Until([&]{return !robot.Ask('U').code;});Until([&]{return robot.Ask().callbacks>5;});
    robot.Ask('M');auto before=pc.Ask('M'),recv_before=robot.Ask();const auto start=Clock::now();std::this_thread::sleep_for(6s);auto after=pc.Ask(),got=robot.Ask();double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::cout<<"rate="<<(rate?100:50)<<" duration="<<seconds<<" publish_hz="<<(after.published-before.published)/seconds<<" send_hz="<<(after.sent-before.sent)/seconds<<" receive_hz="<<(got.received-recv_before.received)/seconds<<" callback_hz="<<got.callbacks/seconds<<" skipped="<<after.skipped-before.skipped<<" missed_packets="<<got.missed-recv_before.missed<<" send_dropped="<<after.dropped-before.dropped<<" receive_dropped="<<got.dropped-recv_before.dropped<<" duplicate="<<got.duplicate<<" reverse="<<got.reverse<<'\n';
    Distribution("age",got.age);Distribution("interval",got.interval);Distribution("send_lateness",after.send_lateness);
    std::cout<<"cpu_percent_sender="<<100*(after.cpu_seconds-before.cpu_seconds)/seconds<<" receiver="<<100*(got.cpu_seconds-recv_before.cpu_seconds)/seconds<<" target95="<<(!rate||((after.sent-before.sent)/seconds>=95)?"PASS":"FAIL")<<'\n';
    assert(got.callbacks>0&&!got.duplicate&&!got.reverse);assert(after.sent>before.sent&&got.received>recv_before.received);
    // Not a hard realtime threshold: explicitly ensure override is not clamped to 50.
    assert((after.sent-before.sent)/seconds>(rate?60:20));
    pc.Ask('N');std::this_thread::sleep_for(1s);auto stopped=robot.Ask();auto local_before=pc.Ask();std::this_thread::sleep_for(300ms);assert(robot.Ask().callbacks==stopped.callbacks);assert(pc.Ask().local>local_before.local);std::cout<<"disconnected_latest_age_us="<<(Now()-stopped.stamp)/1000.0<<" no_new_callback=PASS local_SHM_continues=PASS\n";
    // Local proxy retains last sample. Age grows; application must enforce timeout.
    auto old_session=after.session;assert(!pc.Ask('R').code);assert(pc.Ask().session!=old_session);
    Until([&]{return robot.Ask().callbacks>stopped.callbacks;});
    pc.Ask('F');std::this_thread::sleep_for(200ms);auto frozen=robot.Ask();std::this_thread::sleep_for(300ms);assert(robot.Ask().callbacks==frozen.callbacks);
    // A new route can forward the retained latest sample after restart. Observe, do not mask it.
    pc.Ask('N');std::this_thread::sleep_for(600ms);assert(!pc.Ask('R').code);std::this_thread::sleep_for(1s);auto replay=robot.Ask();
    if(fresh)assert(replay.callbacks==frozen.callbacks);
    std::cout<<"restart_without_new_publish callbacks_delta="<<replay.callbacks-frozen.callbacks<<" payload_sequence="<<replay.last<<" prior="<<frozen.last<<" age_us="<<(Now()-replay.stamp)/1000.0<<" no_stale_republish="<<(replay.callbacks==frozen.callbacks?"PASS":"FAIL")<<" (application freshness/sequence guard required)\n";
    robot.Ask('O');pc.Ask('P');std::this_thread::sleep_for(2s);auto idle=pc.Ask();std::this_thread::sleep_for(300ms);assert(pc.Ask().sent==idle.sent);
    robot.End();pc.End();assert(Entries("/proc/self/fd")==fds);assert(Entries("/dev/shm","rate_h")==0);std::cout<<"child wait/parent FD/owned Topic SHM cleanup PASS\n";std::cout<<"no-subscriber/no-new-sample no-send, gateway Stop/local SHM independence, new session recovery PASS\n";
}
static void FreshCases(){
    fresh=true;initial_frozen=true;Host pc(1,true),robot(2,true);
    Until([&]{return robot.Ask().proxies==1;});Until([&]{return !robot.Ask('U').code;});Until([&]{return pc.Ask().routes==1;});
    assert(robot.Ask().callbacks==0);pc.Ask('V');Until([&]{return robot.Ask().callbacks==1;});
    std::this_thread::sleep_for(500ms);assert(robot.Ask().callbacks==1);
    pc.Ask('V');Until([&]{return robot.Ask().callbacks==2;}); // Identical bytes, new commit
    // A new Consumer registration/route must not replay the stored commit.
    robot.Ask('O');std::this_thread::sleep_for(100ms);auto sent=pc.Ask().sent;assert(!robot.Ask('U').code);
    // Create may deliver the existing Local proxy snapshot once; exclude that
    // local read, then require no additional Proxy publish from the new route.
    std::this_thread::sleep_for(500ms);auto before=robot.Ask();
    std::this_thread::sleep_for(500ms);assert(robot.Ask().callbacks==before.callbacks&&pc.Ask().sent==sent);
    pc.Ask('V');Until([&]{return robot.Ask().callbacks>before.callbacks;});
    auto session=pc.Ask().session;pc.Ask('N');std::this_thread::sleep_for(2100ms);before=robot.Ask();assert(!pc.Ask('R').code);assert(pc.Ask().session!=session);
    Until([&]{return pc.Ask().routes==1;});std::this_thread::sleep_for(500ms);assert(robot.Ask().callbacks==before.callbacks&&pc.Ask().sent==0);
    pc.Ask('V');Until([&]{return robot.Ask().callbacks>before.callbacks;});
    // Test-only blocked read slots: a failed initial snapshot is not empty SHM.
    pc.Ask('N');pc.Ask('L');before=robot.Ask();assert(!pc.Ask('R').code);
    std::this_thread::sleep_for(700ms);assert(pc.Ask().sent==0&&pc.Ask().routes==0&&robot.Ask().callbacks==before.callbacks);
    pc.Ask('J');Until([&]{return pc.Ask().routes==1;});std::this_thread::sleep_for(300ms);assert(pc.Ask().sent==0);
    pc.Ask('V');Until([&]{return robot.Ask().callbacks>before.callbacks;});
    // Replaced Publisher registration/storage needs an independent baseline.
    before=robot.Ask();pc.Ask('X');std::this_thread::sleep_for(800ms);assert(robot.Ask().callbacks==before.callbacks);
    pc.Ask('V');Until([&]{return robot.Ask().callbacks>before.callbacks;});
    robot.End();pc.End();assert(Entries("/dev/shm","rate_h")==0);
    initial_frozen=false;
    std::cout<<"fresh fail-closed contention / empty first commit / identical payload new commit / repeated interest / consumer re-registration / 2s stale restart / publisher replacement / receiver proxy no replay PASS\n";
}

static void MultiRoutes(){fresh=true;publish_hz=200;const auto fds=Entries("/proc/self/fd");Host pc(1,true),a(2,true),b(3,true);
    Until([&]{return a.Ask().proxies==1&&b.Ask().proxies==1;});
    Until([&]{return !a.Ask('U').code&&!b.Ask('U').code;});Until([&]{return pc.Ask().routes==2&&a.Ask().callbacks>5&&b.Ask().callbacks>5;});
    a.Ask('M');b.Ask('M');auto before=pc.Ask('M');const auto start=Clock::now();std::this_thread::sleep_for(6s);
    auto after=pc.Ask(),ra=a.Ask(),rb=b.Ask();double seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::cout<<"multi_route send_hz="<<(after.sent-before.sent)/seconds<<" callback_a_hz="<<ra.callbacks/seconds<<" callback_b_hz="<<rb.callbacks/seconds<<'\n';
    Distribution("multi_a_interval",ra.interval);Distribution("multi_b_interval",rb.interval);Distribution("multi_send_lateness",after.send_lateness);
    assert(ra.callbacks>400&&rb.callbacks>400&&!ra.duplicate&&!rb.duplicate&&!ra.reverse&&!rb.reverse);
    a.End();b.End();pc.End();assert(Entries("/proc/self/fd")==fds&&Entries("/dev/shm","rate_h")==0);
}

static void WatchdogCases(){
    fresh=true;initial_frozen=true;Host pc(1,true),robot(2,true);
    using State=kcf::CommandFreshnessState;
    auto state=[&]{return static_cast<State>(robot.Ask().watchdog_state);};
    Until([&]{return robot.Ask().proxies==1;});assert(!robot.Ask('W').code);
    Until([&]{return pc.Ask().routes==1;});assert(state()==State::INVALID);
    pc.Ask('V');Until([&]{return state()==State::VALID;});assert(robot.Ask().command_output==123);
    pc.Ask('P');const auto continuous_until=Clock::now()+1s;
    while(Clock::now()<continuous_until){assert(state()==State::VALID);std::this_thread::sleep_for(1ms);}
    pc.Ask('F');
    std::this_thread::sleep_for(150ms);assert(state()==State::EXPIRED&&robot.Ask().command_output==0);
    pc.Ask('V');Until([&]{return state()==State::VALID;});
    auto session=pc.Ask().session;pc.Ask('N');std::this_thread::sleep_for(150ms);assert(state()==State::EXPIRED&&robot.Ask().command_output==0);
    assert(!pc.Ask('R').code&&pc.Ask().session!=session);Until([&]{return pc.Ask().routes==1;});
    std::this_thread::sleep_for(150ms);assert(state()==State::EXPIRED&&robot.Ask().command_output==0);
    pc.Ask('V');Until([&]{return state()==State::VALID;});
    robot.Ask('O');assert(!robot.Ask('W').code);Until([&]{return pc.Ask().routes==1;});
    std::this_thread::sleep_for(250ms);assert(state()==State::INVALID&&robot.Ask().command_output==0);
    pc.Ask('V');Until([&]{return state()==State::VALID;});
    robot.End();pc.End();assert(Entries("/dev/shm","rate_h")==0);
    std::cout<<"Network proxy NEXT / continuous 100Hz commits / watchdog expiry / zero output / new session stale suppression / same bytes new commit recovery / reconnect INVALID / cleanup PASS\n";
}

int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);alarm(180);
    if(argc==2&&std::string(argv[1])=="--watchdog") {WatchdogCases();return 0;}
    if(argc==2&&std::string(argv[1])=="--multi") {MultiRoutes();return 0;}
    if(argc==2&&std::string(argv[1])=="--rates") {fresh=true;Run(false);Run(true);publish_hz=200;Run(true);return 0;}
    auto r=Parse({"x","--topic-rate","/robot/cmd_vel:100"});auto c=gateway::Config(r,{"lo","127.0.0.1",1,false});assert(!c.topics.enabled&&c.topics.max_hz==50&&c.topics.rate_limits[0].max_hz==100);
    for(auto value:{"x:100","/x:0","/x:nan","/x:1001","/x:100tail"}){bool failed=false;try{Parse({"x","--topic-rate",value});}catch(...){failed=true;}assert(failed);}
    bool duplicate=false;try{Parse({"x","--topic-rate","/x:50","--topic-rate","/x:100"});}catch(...){duplicate=true;}assert(duplicate);
    auto rx=gateway::Config(Parse({"x","--remote-topic","/robot/cmd_vel"}),{"lo","127.0.0.1",1,false});assert(rx.topics.enabled&&!rx.topic_observer_enabled&&rx.metadata.remote_topics.size()==1&&rx.parameter_set_clients.empty()&&rx.service_call_clients.empty()&&rx.action_goal_clients.empty());
    std::cout<<"CLI rate replacement / explicit legacy receive / default deny PASS\n";Run(false);Run(true);fresh=true;Run(false);Run(true);FreshCases();
}
