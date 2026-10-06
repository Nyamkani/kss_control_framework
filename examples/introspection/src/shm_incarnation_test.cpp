#ifdef NDEBUG
#undef NDEBUG
#endif
#include "r3_test_types.hpp"
#include "kcf/dynamic/dynamic_topic_reader.hpp"
#include "kcf/dynamic/dynamic_parameter_client.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/parameter/parameter.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Link wrapping belongs only to this test. Kill an observer while it owns the
// temporary validation fd; production code contains no injection hooks.
bool stop_in_stat=false;
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_fstat(int fd,struct stat* st) {
    if(stop_in_stat) { stop_in_stat=false; raise(SIGSTOP); }
    return __real_fstat(fd,st);
}
namespace {
using namespace kcf;
TestData Sample(std::uint64_t n) {return {n,float(n),int(n),{1,2,3,float(n)}};}
DynamicPayload Bytes(const TestData& value) {
    DynamicPayload p;p.type_id=TypeDescriptorTraits<TestData>::Get().type_id;
    p.bytes.resize(sizeof(value));std::memcpy(p.bytes.data(),&value,sizeof(value));return p;
}
void Same(const DynamicPayload& p,std::uint64_t n) {
    assert(p.type_id==TypeDescriptorTraits<TestData>::Get().type_id);
    assert(p.bytes.size()==sizeof(TestData));TestData v{};std::memcpy(&v,p.bytes.data(),sizeof(v));
    assert(v.sequence==n && v.x==float(n) && v.count==int(n) && v.values[3]==float(n));
}
std::uint64_t StartTicks() {
    std::ifstream file("/proc/self/stat");std::string line;std::getline(file,line);
    std::istringstream fields(line.substr(line.rfind(')')+2));std::string value;
    for(int i=0;i<20;++i)assert(bool(fields>>value));
    return std::stoull(value);
}
std::size_t Fds() {
    auto* dir=opendir("/proc/self/fd");assert(dir);std::size_t n=0;
    while(auto* e=readdir(dir))if(e->d_name[0]!='.')++n;
    closedir(dir);return n;
}
template<class F> double Micros(F f) {
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<1000;++i)f();
    return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/1000;
}
void KillInValidation(const std::string& name,bool topic) {
    const auto pid=fork();assert(pid>=0);
    if(pid==0) {
        auto d=TypeDescriptorTraits<TestData>::Get();DynamicPayload out;
        DynamicTopicReader reader;DynamicParameterClient client;
        assert((topic?reader.Open(name,d):client.Open(name,d))==0);
        stop_in_stat=true;
        (void)(topic?reader.ReadLatest(out):client.Get(out));_exit(3);
    }
    int status{};assert(waitpid(pid,&status,WUNTRACED)==pid && WIFSTOPPED(status));
    assert(kill(pid,SIGKILL)==0);assert(waitpid(pid,&status,0)==pid);
    assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
}
void Topic(const std::string& name) {
    const auto pid=getpid();const auto ticks=StartTicks();const auto d=TypeDescriptorTraits<TestData>::Get();
    Publisher<TestData> a,b;DynamicTopicReader reader;DynamicPayload out;
    assert(a.Create(name)==0 && a.Publish(Sample(1))==0 && reader.Open(name,d)==0);
    assert(reader.ReadLatest(out)==0);Same(out,1);
    assert(a.Close()==0 && a.Unlink()==0);
    assert(b.Create(name)==0 && b.Publish(Sample(2))==0);
    const auto same_type=TypeDescriptorTraits<TestData>::Get();
    assert(getpid()==pid && StartTicks()==ticks && same_type.type_id==d.type_id && same_type.payload_size==d.payload_size);
    const auto fd_count=Fds();
    for(int i=0;i<1000;++i){assert(reader.ReadLatest(out)==-ESTALE);Same(out,1);}
    assert(Fds()==fd_count);
    reader.Close();assert(reader.Open(name,d)==0 && reader.ReadLatest(out)==0);Same(out,2);
    std::cout<<"Topic ReadLatest us/access: "<<Micros([&]{assert(reader.ReadLatest(out)==0);Same(out,2);})<<'\n';
    assert(Fds()==fd_count);
    KillInValidation(name,true);
    for(int i=0;i<1000;++i)assert(b.Publish(Sample(3))==0);
    assert(reader.ReadLatest(out)==0);Same(out,3);
    assert(b.Unlink()==0);assert(reader.ReadLatest(out)==-ESTALE);Same(out,3);
    reader.Close();assert(b.Close()==0);
    std::cout<<"PASS Topic: same PID/ticks/type/size recreate, explicit reopen, unlink, 1000 reads, observer kill\n";
}
void ParameterTest(const std::string& name) {
    const auto pid=getpid();const auto ticks=StartTicks();const auto d=TypeDescriptorTraits<TestData>::Get();
    Parameter<TestData> a,b,old_typed;DynamicParameterClient client;DynamicPayload out;
    assert(a.Create(name,Sample(1))==0 && old_typed.Open(name)==0 && client.Open(name,d)==0);
    assert(client.Get(out)==0);Same(out,1);
    assert(a.Close()==0 && a.Unlink()==0 && b.Create(name,Sample(2))==0);
    assert(getpid()==pid && StartTicks()==ticks);
    const auto fd_count=Fds();auto value=Bytes(Sample(9));
    for(int i=0;i<1000;++i){assert(client.Get(out)==-ESTALE);Same(out,1);assert(client.Set(value)==-ESTALE);}
    assert(Fds()==fd_count);
    TestData typed{};assert(old_typed.Get(typed)==0 && typed.sequence==1); // old mapping not written
    assert(b.Get(typed)==0 && typed.sequence==2); // new mapping not written
    client.Close();assert(client.Open(name,d)==0 && client.Get(out)==0);Same(out,2);
    value=Bytes(Sample(3));assert(client.Set(value)==0 && b.Get(typed)==0 && typed.sequence==3);
    std::cout<<"Parameter Get us/access: "<<Micros([&]{assert(client.Get(out)==0);Same(out,3);})<<'\n';
    std::cout<<"Parameter Set us/access: "<<Micros([&]{assert(client.Set(value)==0);})<<'\n';
    assert(Fds()==fd_count);
    KillInValidation(name,false);
    for(int i=0;i<1000;++i)assert(b.Set(Sample(4))==0 && b.Get(typed)==0 && typed.sequence==4);
    assert(client.Get(out)==0);Same(out,4);
    assert(b.Unlink()==0);assert(client.Get(out)==-ESTALE && client.Set(value)==-ESTALE);Same(out,4);
    assert(b.Get(typed)==0 && typed.sequence==4);
    client.Close();assert(old_typed.Close()==0 && b.Close()==0);
    std::cout<<"PASS Parameter: stale Get/Set preserves both mappings, reopen, 1000 Get/Set, unlink, observer kill\n";
}
}
int main() {
    const auto before=Fds();const auto name="/kcf_r41_"+std::to_string(getpid());
    Topic(name+"/topic");ParameterTest(name+"/parameter");assert(Fds()==before);
    std::cout<<"PASS R4.1: no permanent/temporary fd leak\n";
}
