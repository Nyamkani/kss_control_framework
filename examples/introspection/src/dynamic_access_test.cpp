#ifdef NDEBUG
#undef NDEBUG
#endif
#include "r3_test_types.hpp"
#include "bringup/bringup.hpp"
#include "kcf/dynamic/dynamic_topic_reader.hpp"
#include "kcf/dynamic/dynamic_parameter_client.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/parameter/parameter.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
using namespace std::chrono_literals;
struct OtherData {std::uint64_t words[4];};
struct Bytes3 {std::uint8_t values[3];};
struct alignas(64) AlignedData {std::uint64_t words[8];};
namespace kcf {
template<> struct TypeDescriptorTraits<OtherData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {auto d=MakeTypeDescriptor<OtherData>("kcf.r4.Other.v1");d.field_count=1;d.fields[0]=MakeField<OtherData,std::uint64_t[4]>("words",offsetof(OtherData,words));return d;}
};
template<> struct TypeDescriptorTraits<Bytes3> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {auto d=MakeTypeDescriptor<Bytes3>("kcf.r4.Bytes3.v1");d.field_count=1;d.fields[0]=MakeField<Bytes3,std::uint8_t[3]>("values",offsetof(Bytes3,values));return d;}
};
template<> struct TypeDescriptorTraits<AlignedData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {auto d=MakeTypeDescriptor<AlignedData>("kcf.r4.Aligned.v1");d.field_count=1;d.fields[0]=MakeField<AlignedData,std::uint64_t[8]>("words",offsetof(AlignedData,words));return d;}
};
}
namespace {
// Test-only link wrapper: stop inside the REAL dynamic byte copy, after its
// slot pin / robust lock. No production failure injection hooks.
bool stop_copy=false;int copy_notice=-1;
void Send(int fd,int n){assert(write(fd,&n,sizeof(n))==sizeof(n));}
int Receive(int fd){pollfd p{fd,POLLIN,0};assert(poll(&p,1,10000)==1);int n;assert(read(fd,&n,sizeof(n))==sizeof(n));return n;}
void Reap(pid_t pid,int signal=0){int s;assert(waitpid(pid,&s,0)==pid);if(signal)assert(WIFSIGNALED(s)&&WTERMSIG(s)==signal);else assert(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(5ms);}}
template<class T> kcf::DynamicPayload Payload(const T& v){kcf::DynamicPayload p;p.type_id=kcf::TypeDescriptorTraits<T>::Get().type_id;p.bytes.resize(sizeof(T));std::memcpy(p.bytes.data(),&v,sizeof(T));return p;}
template<class T> T Decode(const kcf::DynamicPayload& p){assert(p.bytes.size()==sizeof(T));T v;std::memcpy(&v,p.bytes.data(),sizeof(v));return v;}
TestData Sample(std::uint64_t n){return {n,float(n),std::int32_t(n),{float(n),float(n+1),float(n+2),float(n+3)}};}
void Same(const TestData& a,const TestData& b){assert(a.sequence==b.sequence&&a.x==b.x&&a.count==b.count);for(int i=0;i<4;++i)assert(a.values[i]==b.values[i]);}
std::string base;
void Basic(){
    using namespace kcf;auto d=TypeDescriptorTraits<TestData>::Get();auto wrong=TypeDescriptorTraits<OtherData>::Get();
    static_assert(sizeof(TestData)==sizeof(OtherData));
    Publisher<TestData> pub;assert(pub.Create(base+"_topic")==0);
    DynamicTopicReader reader;DynamicPayload out=Payload(Sample(99));
    assert(reader.ReadLatest(out)==-EBADF);assert(reader.Open(base+"_topic",0,d)==-EINVAL);
    assert(reader.Open(base+"_topic",wrong)==-EPROTOTYPE);
    auto bad=d;bad.fields[0].offset=UINT32_MAX;assert(reader.Open(base+"_topic",bad)==-EINVAL);
    auto conflict=TypeDescriptorTraits<ConflictData>::Get();assert(reader.Open(base+"_topic",conflict)==-EPROTOTYPE);
    assert(reader.Open(base+"_topic",d)==0);assert(reader.Open(base+"_topic",d)==-EBUSY);
    assert(reader.ReadLatest(out)==-EAGAIN);Same(Decode<TestData>(out),Sample(99));
    assert(pub.Publish(Sample(1))==0);assert(reader.ReadLatest(out)==0);Same(Decode<TestData>(out),Sample(1));auto seq=out.sequence;
    assert(pub.Publish(Sample(2))==0);assert(reader.ReadLatest(out)==0);Same(Decode<TestData>(out),Sample(2));assert(out.sequence>seq);
    reader.Close();reader.Close();assert(reader.ReadLatest(out)==-EBADF);assert(pub.Close()==0&&pub.Unlink()==0);
    // Same name, different semantic type of equal size: stale metadata rejected.
    Publisher<OtherData> replacement;assert(replacement.Create(base+"_topic")==0);
    assert(reader.Open(base+"_topic",d)==-EPROTOTYPE);assert(replacement.Close()==0&&replacement.Unlink()==0);
    Publisher<UnknownData> legacy;assert(legacy.Create(base+"_unknown")==0&&legacy.Publish({3})==0);
    auto unknown=MakeTypeDescriptor<UnknownData>("kcf.r4.Unknown.v1");unknown.field_count=1;unknown.fields[0]=MakeField<UnknownData,int>("value",0);
    assert(reader.Open(base+"_unknown",unknown)==-EPROTOTYPE);unknown.type_id=0;assert(reader.Open(base+"_unknown",unknown)==-EINVAL);
    assert(legacy.Close()==0&&legacy.Unlink()==0);
    Parameter<UnknownData> legacy_parameter;assert(legacy_parameter.Create(base+"_unknown_param",{1})==0);
    assert(legacy_parameter.Set({2})==0);UnknownData uv{};assert(legacy_parameter.Get(uv)==0&&uv.value==2);
    unknown.type_id=StableTypeId(unknown.type_name);DynamicParameterClient unknown_client;
    assert(unknown_client.Open(base+"_unknown_param",unknown)==-EPROTOTYPE);
    assert(legacy_parameter.Close()==0&&legacy_parameter.Unlink()==0);
    std::atomic<int> observed{0};Parameter<TestData> owner;
    assert(owner.Create(base+"_param",Sample(10),[&](const TestData& value){observed.store(value.count);})==0);
    DynamicParameterClient client;assert(client.Get(out)==-EBADF&&client.Set(out)==-EBADF);
    assert(client.Open(base+"_param",wrong)==-EPROTOTYPE);assert(client.Open(base+"_param",d)==0);
    assert(client.Get(out)==0);Same(Decode<TestData>(out),Sample(10));assert(out.sequence==0);
    auto fresh=Payload(Sample(20));auto invalid=fresh;invalid.type_id=wrong.type_id;
    assert(client.Set(invalid)==-EPROTOTYPE);invalid=fresh;invalid.bytes.pop_back();assert(client.Set(invalid)==-EMSGSIZE);
    assert(client.Get(out)==0&&out.sequence==0);Same(Decode<TestData>(out),Sample(10));
    assert(client.Set(fresh)==0);Until([&]{return observed.load()==20;});
    TestData typed{};std::uint64_t version;assert(owner.Get(typed,&version)==0&&version==1);Same(typed,Sample(20));
    assert(client.Get(out)==0&&out.sequence==1);Same(Decode<TestData>(out),typed);
    assert(owner.Close()==0&&owner.Unlink()==0);client.Close();
    std::cout<<"PASS Topic A/B sequence, equal-size type/layout mismatch, stale-name rejection, undefined Topic, Parameter Get/Set/notification and failed-write isolation\n";
}
template<class T> void Layout(const std::string& name,const T& v){
    using namespace kcf;Publisher<T> pub;Parameter<T> param;assert(pub.Create(name)==0&&param.Create(name+"p",v)==0);
    auto d=TypeDescriptorTraits<T>::Get();DynamicTopicReader reader;DynamicParameterClient client;assert(reader.Open(name,d)==0&&client.Open(name+"p",d)==0);
    assert(pub.Publish(v)==0);DynamicPayload out;assert(reader.ReadLatest(out)==0);assert(std::memcmp(out.bytes.data(),&v,sizeof(v))==0);
    assert(client.Get(out)==0);assert(std::memcmp(out.bytes.data(),&v,sizeof(v))==0);assert(client.Set(Payload(v))==0);
    reader.Close();client.Close();assert(pub.Close()==0&&pub.Unlink()==0&&param.Close()==0&&param.Unlink()==0);
}
void TopicConcurrent(){
    using namespace kcf;Publisher<TestData> pub;assert(pub.Create(base+"_stream")==0);
    DynamicTopicReader reader;assert(reader.Open(base+"_stream",TypeDescriptorTraits<TestData>::Get())==0);
    std::atomic<bool> done{false};std::atomic<unsigned> copied{0};
    std::thread observer([&]{DynamicPayload p;std::uint64_t previous=0;while(!done.load()){
        const int result=reader.ReadLatest(p);assert(result==0||result==-EAGAIN);
        if(!result){auto v=Decode<TestData>(p);Same(v,Sample(v.sequence));assert(p.sequence>=previous);previous=p.sequence;copied.fetch_add(1);}
    }});
    for(unsigned i=1;i<=10000;++i)assert(pub.Publish(Sample(i))==0);
    Until([&]{return copied.load()>0;});done.store(true);observer.join();
    DynamicPayload last;assert(reader.ReadLatest(last)==0&&last.sequence==10000);Same(Decode<TestData>(last),Sample(10000));
    reader.Close();assert(pub.Close()==0&&pub.Unlink()==0);
    std::cout<<"PASS concurrent Topic snapshots without torn bytes, all 10000 publishes successful\n";
}
void Concurrent(){
    using namespace kcf;Parameter<TestData> owner;assert(owner.Create(base+"_concurrent",Sample(0))==0);
    DynamicParameterClient client;assert(client.Open(base+"_concurrent",TypeDescriptorTraits<TestData>::Get())==0);
    auto typed=[&]{for(int i=1;i<=2000;++i){assert(owner.Set(Sample(i))==0);TestData v;assert(owner.Get(v)==0);Same(v,Sample(v.sequence));}};
    auto dynamic=[&]{for(int i=2001;i<=4000;++i){assert(client.Set(Payload(Sample(i)))==0);DynamicPayload p;assert(client.Get(p)==0);auto v=Decode<TestData>(p);Same(v,Sample(v.sequence));}};
    std::thread a(typed),b(dynamic),c(dynamic);a.join();b.join();c.join();
    DynamicPayload out;assert(client.Get(out)==0&&out.sequence==6000);client.Close();assert(owner.Close()==0&&owner.Unlink()==0);
    std::cout<<"PASS concurrent typed/dynamic Get/Set, consistent bytes and 6000 versions\n";
}
void Crash(){
    using namespace kcf;auto d=TypeDescriptorTraits<TestData>::Get();Publisher<TestData> pub;Parameter<TestData> param;
    assert(pub.Create(base+"_crash_topic")==0&&pub.Publish(Sample(1))==0);assert(param.Create(base+"_crash_param",Sample(1))==0);
    for(int mode=0;mode<3;++mode){
        int ready[2];assert(pipe(ready)==0);auto child=fork();assert(child>=0);
        if(!child){close(ready[0]);copy_notice=ready[1];DynamicPayload out=Payload(Sample(9));
            if(mode==0){DynamicTopicReader r;assert(r.Open(base+"_crash_topic",d)==0);stop_copy=true;(void)r.ReadLatest(out);}
            else {DynamicParameterClient c;assert(c.Open(base+"_crash_param",d)==0);stop_copy=true;if(mode==1)(void)c.Get(out);else(void)c.Set(out);}
            _exit(2);
        }
        close(ready[1]);assert(Receive(ready[0])==1);close(ready[0]);int status;assert(waitpid(child,&status,WUNTRACED)==child&&WIFSTOPPED(status));
        if(mode==0)for(int i=0;i<1000;++i)assert(pub.Publish(Sample(i))==0); // reader paused with actual pin
        assert(kill(child,SIGKILL)==0);Reap(child,SIGKILL);
        if(mode==0)for(int i=0;i<1000;++i)assert(pub.Publish(Sample(i))==0);
        else {TestData v{};assert(param.Get(v)==0);Same(v,Sample(1));assert(param.Set(Sample(1))==0);}
    }
    assert(pub.Close()==0&&pub.Unlink()==0&&param.Close()==0&&param.Unlink()==0);
    std::cout<<"PASS SIGSTOP/SIGKILL inside Topic copy; robust Parameter Get death and interrupted Set rollback\n";
}
void Corruption(){
    using namespace kcf;Parameter<TestData> owner;auto d=TypeDescriptorTraits<TestData>::Get();assert(owner.Create(base+"_corrupt",Sample(5))==0);
    DynamicParameterClient client;assert(client.Open(base+"_corrupt",d)==0);
    int fd=shm_open((base+"_corrupt").c_str(),O_RDWR,0600);assert(fd>=0);
    detail::OwnerHeader header{};assert(pread(fd,&header,sizeof(header),0)==sizeof(header));auto original=header;
    DynamicPayload unchanged=Payload(Sample(99));
    for(int mode=0;mode<6;++mode){header=original;
        if(mode==0)header.type_id++;if(mode==1)header.layout_id++;if(mode==2)header.size++;
        if(mode==3)header.alignment*=2;if(mode==4)header.format=2;if(mode==5)header.magic=0;
        assert(pwrite(fd,&header,sizeof(header),0)==sizeof(header));
        assert(client.Set(Payload(Sample(9)))<0&&client.Get(unchanged)<0);Same(Decode<TestData>(unchanged),Sample(99));
        DynamicParameterClient rejected;assert(rejected.Open(base+"_corrupt",d)<0);
        assert(pwrite(fd,&original,sizeof(original),0)==sizeof(original));TestData v;std::uint64_t version;
        assert(owner.Get(v,&version)==0&&version==0);Same(v,Sample(5));
    }
    // Complete old format is rejected by typed Open/reclaim as well.
    header=original;header.format=2;assert(pwrite(fd,&header,sizeof(header),0)==sizeof(header));
    Parameter<TestData> typed;assert(typed.Open(base+"_corrupt")==-EPROTO);assert(typed.Create(base+"_corrupt",Sample(7))==-EEXIST);
    assert(pwrite(fd,&original,sizeof(original),0)==sizeof(original));close(fd);client.Close();assert(owner.Close()==0&&owner.Unlink()==0);
    std::cout<<"PASS current storage revalidation, malformed layout/header rejection, old-format rejection and value preservation\n";
}
struct Element:kcf::ProcessElement {
    std::string name;kcf::Publisher<TestData> pub;kcf::Parameter<TestData> param;kcf::Publisher<UnknownData> legacy;
    explicit Element(std::string n):name(std::move(n)){}
    int Setup() override{assert(pub.Create(name+"_topic")==0&&param.Create(name+"_param",Sample(1))==0&&legacy.Create(name+"_legacy")==0);return 0;}
    int Loop() override{TestData v;assert(param.Get(v)==0);assert(pub.Publish(v)==0&&legacy.Publish({1})==0);return 0;}
    void Shutdown() override{assert(pub.Close()==0&&pub.Unlink()==0&&param.Close()==0&&param.Unlink()==0&&legacy.Close()==0&&legacy.Unlink()==0);}
};
void Chain(const char* executable){
    using namespace kcf;auto supervisor=fork();assert(supervisor>=0);
    if(!supervisor){bringup::Bringup app;bringup::ElementSpec spec;spec.name="r4";spec.executable=executable;spec.arguments={"--child",base+"_chain"};if(app.Setup("r4_application",{spec}))_exit(2);_exit(app.Run());}
    IntrospectionClient c;RuntimeInfo runtime{};
    Until([&]{std::vector<SupervisorInfo> supervisors;assert(c.ListSupervisors(supervisors)==0);for(const auto& s:supervisors)if(s.pid==supervisor&&s.element_count==1&&s.application_state==ApplicationState::RUNNING){std::vector<RuntimeInfo> runtimes;assert(c.ListRuntimes(runtimes)==0);for(const auto& r:runtimes)if(r.pid==s.elements[0].pid&&r.process_start_ticks==s.elements[0].process_start_ticks&&r.state==ProcessState::RUNNING){runtime=r;return true;}}return false;});
    std::vector<EndpointInfo> endpoints;assert(c.ListEndpoints(runtime,endpoints)==0);DynamicTopicReader reader;DynamicParameterClient client;bool topic=false,param=false,legacy=false;
    for(const auto& e:endpoints){if(std::string(e.name)==base+"_chain_legacy"){assert(!e.type_id);legacy=true;continue;}
        TypeDescriptor d;assert(e.type_id&&c.GetType(runtime,e.type_id,d)==0);
        if(e.kind==EndpointKind::TOPIC){assert(reader.Open(e.name,e.type_id,d)==0);topic=true;}
        if(e.kind==EndpointKind::PARAMETER){assert(client.Open(e.name,e.type_id,d)==0);param=true;}
    }assert(topic&&param&&legacy);
    DynamicPayload p;assert(client.Get(p)==0);Same(Decode<TestData>(p),Sample(1));
    Until([&]{return reader.ReadLatest(p)==0;});Same(Decode<TestData>(p),Sample(1));auto seq=p.sequence;
    assert(client.Set(Payload(Sample(7)))==0);
    Until([&]{return reader.ReadLatest(p)==0&&Decode<TestData>(p).sequence==7;});Same(Decode<TestData>(p),Sample(7));assert(p.sequence>seq);
    client.Close();reader.Close();assert(kill(supervisor,SIGTERM)==0);Reap(supervisor);
    std::cout<<"PASS Supervisor -> Runtime -> Endpoint -> Type -> Topic bytes / Parameter Get-Set\n";
}
}
extern "C" void* __real_memcpy(void*,const void*,std::size_t);
extern "C" void* __wrap_memcpy(void* dest,const void* source,std::size_t size){
    if(stop_copy&&size==sizeof(TestData)){stop_copy=false;Send(copy_notice,1);raise(SIGSTOP);}
    return __real_memcpy(dest,source,size);
}
int main(int argc,char** argv){
    if(argc==3&&std::string(argv[1])=="--child"){Element e(argv[2]);kcf::ProcessRuntime r;r.SetLoopFrequency(500);return r.Run(e)?1:0;}
    alarm(90);base="/r4_"+std::to_string(getpid());
    Basic();Layout(base+"/bytes%3",Bytes3{{1,2,3}});Layout(base+"_aligned",AlignedData{{1,2,3,4,5,6,7,8}});
    std::cout<<"PASS small/over-aligned payload layout and nested logical names\n";
    TopicConcurrent();Concurrent();Crash();Corruption();Chain(argv[0]);std::cout<<"R4 dynamic data access PASS\n";
}
