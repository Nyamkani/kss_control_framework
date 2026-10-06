#ifdef NDEBUG
#undef NDEBUG
#endif
#include "r3_test_types.hpp"
#include "bringup/bringup.hpp"
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <memory>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
void Send(int fd,int n){assert(write(fd,&n,sizeof(n))==sizeof(n));}
int Receive(int fd){pollfd p{fd,POLLIN,0};assert(poll(&p,1,10000)==1);int n;assert(read(fd,&n,sizeof(n))==sizeof(n));return n;}
template<class F> void Until(F f){auto end=std::chrono::steady_clock::now()+10s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(10ms);}}
kcf::RuntimeInfo Runtime(pid_t pid){kcf::RuntimeInfo r;Until([&]{kcf::IntrospectionClient c;std::vector<kcf::RuntimeInfo> all;assert(c.ListRuntimes(all)==0);for(const auto& v:all)if(v.pid==pid&&v.state==kcf::ProcessState::RUNNING){r=v;return true;}return false;});return r;}
struct Element:kcf::ProcessElement {
    int command=-1,reply=-1;std::string base;
    kcf::Publisher<TestData> publisher;kcf::Subscriber<TestData> subscriber;kcf::Parameter<TestData> parameter;
    kcf::Parameter<UnknownData> unknown_parameter;
    kcf::Publisher<UnknownData> unknown;kcf::Parameter<BadData> bad;kcf::Publisher<ConflictData> conflict;
    kcf::Publisher<NestedData> nested;kcf::Publisher<ExtraData> extra;kcf::Parameter<CapacityData> full;
    explicit Element(std::string b):base(std::move(b)){}
    int Setup() override {
        assert(publisher.Create(base+"_topic")==0);assert(subscriber.Create(base+"_topic",[](const auto&){})==0);
        assert(parameter.Create(base+"_parameter",{})==0);
        assert(unknown_parameter.Create(base+"_unknown_parameter",{2})==0);
        assert(unknown_parameter.Set({6})==0);UnknownData unknown_value{};assert(unknown_parameter.Get(unknown_value)==0&&unknown_value.value==6);
        assert(unknown.Create(base+"_unknown")==0);assert(bad.Create(base+"_bad",{1})==0);
        assert(conflict.Create(base+"_conflict")==0);assert(nested.Create(base+"_nested")==0);
        assert(publisher.Publish({7,1,2,{3,4,5,6}})==0);
        assert(unknown.Publish({9})==0);assert(conflict.Publish({})==0);
        assert(bad.Set({8})==0);BadData value{};assert(bad.Get(value)==0&&value.value==8);
        return 0;
    }
    int Loop() override {
        if(command<0)return 0;pollfd p{command,POLLIN,0};if(poll(&p,1,0)!=1)return 0;
        int action=Receive(command);
        if(action==1) {assert(extra.Create(base+"_extra")==0);assert(extra.Publish({5})==0);}
        if(action==2) {assert(extra.Close()==0);assert(extra.Unlink()==0);assert(extra.Create(base+"_extra")==0);}
        if(action==3) {
            unsigned accepted=0;
            for(unsigned i=0;i<70;++i){auto d=kcf::TypeDescriptorTraits<TestData>::Get();
                std::string name="kcf.test.Capacity"+std::to_string(i)+".v1";
                std::memset(d.type_name,0,sizeof(d.type_name));std::memcpy(d.type_name,name.data(),name.size());d.type_id=kcf::StableTypeId(name);
                if(kcf::detail::RegisterType(d))++accepted;
            }
            assert(accepted==61); // TestData, NestedData, ExtraData already exist
            assert(full.Create(base+"_full",{})==0);assert(full.Set({7})==0);
            CapacityData v{};assert(full.Get(v)==0&&v.value==7);
        }
        assert(publisher.Publish({})==0);assert(unknown.Publish({4})==0);assert(bad.Set({4})==0);
        Send(reply,action);return 0;
    }
    void Shutdown() override {
        assert(subscriber.Close()==0);assert(publisher.Close()==0);assert(publisher.Unlink()==0);
        assert(parameter.Close()==0);assert(parameter.Unlink()==0);assert(unknown.Close()==0);assert(unknown.Unlink()==0);
        assert(unknown_parameter.Close()==0);assert(unknown_parameter.Unlink()==0);
        assert(bad.Close()==0);assert(bad.Unlink()==0);assert(conflict.Close()==0);assert(conflict.Unlink()==0);
        assert(nested.Close()==0);assert(nested.Unlink()==0);
        extra.Close();extra.Unlink();full.Close();full.Unlink();
    }
};
void Check(const kcf::RuntimeInfo& r,const std::string& base){
    kcf::IntrospectionClient c;std::vector<kcf::EndpointInfo> endpoints;assert(c.ListEndpoints(r,endpoints)==0);
    unsigned shared=0;for(const auto& e:endpoints){std::string name=e.name;
        if(name==base+"_topic"||name==base+"_parameter"){assert(e.type_id==kcf::StableTypeId("kcf.test.TestData.v1"));++shared;}
        if(name==base+"_unknown"||name==base+"_unknown_parameter"||name==base+"_bad"||name==base+"_conflict")assert(!e.type_id && e.diagnostic_type_name[0]);
    }assert(shared==3);
    kcf::TypeDescriptor d;assert(c.GetType(r,kcf::StableTypeId("kcf.test.TestData.v1"),d)==0);
    const auto expected=kcf::TypeDescriptorTraits<TestData>::Get();
    assert(d.payload_size==sizeof(TestData)&&d.payload_alignment==alignof(TestData)&&d.field_count==4);
    for(unsigned i=0;i<4;++i){auto& a=d.fields[i];const auto& b=expected.fields[i];
        assert(std::strcmp(a.name,b.name)==0&&a.kind==b.kind&&a.offset==b.offset&&a.element_size==b.element_size&&a.array_count==b.array_count);}
    assert(c.GetType(r,kcf::StableTypeId("kcf.test.NestedData.v1"),d)==0);
    assert(std::string(d.fields[1].name)=="header.timestamp_us"&&d.fields[1].offset==offsetof(NestedData,header)+offsetof(Header,timestamp_us));
}
// Test-only slot access to hold reader pins without production hooks.
using Payload = kcf::detail::TypeRegistrySnapshot;
using Slot = kcf::detail::ChannelSlot<Payload>;
}
int main(int argc,char** argv){
    if(argc==3&&std::string(argv[1])=="--child"){Element e(argv[2]);kcf::ProcessRuntime r;r.SetLoopFrequency(500);return r.Run(e)?1:0;}
    alarm(90);using namespace kcf;
    static_assert(StableTypeId("hello")==0xa430d84680aabd0bull);
    assert(StableTypeId("kcf.test.TestData.v1")!=StableTypeId("kcf.test.TestData.v2"));
    struct Primitives { bool b; std::int8_t i8; std::uint8_t u8; std::int16_t i16; std::uint16_t u16;
        std::int32_t i32; std::uint32_t u32; std::int64_t i64; std::uint64_t u64; float f32; double f64; };
    auto primitives=MakeTypeDescriptor<Primitives>("kcf.test.Primitives.v1");primitives.field_count=11;
    primitives.fields[0]=MakeField<Primitives,bool>("b",offsetof(Primitives,b));
    primitives.fields[1]=MakeField<Primitives,std::int8_t>("i8",offsetof(Primitives,i8));
    primitives.fields[2]=MakeField<Primitives,std::uint8_t>("u8",offsetof(Primitives,u8));
    primitives.fields[3]=MakeField<Primitives,std::int16_t>("i16",offsetof(Primitives,i16));
    primitives.fields[4]=MakeField<Primitives,std::uint16_t>("u16",offsetof(Primitives,u16));
    primitives.fields[5]=MakeField<Primitives,std::int32_t>("i32",offsetof(Primitives,i32));
    primitives.fields[6]=MakeField<Primitives,std::uint32_t>("u32",offsetof(Primitives,u32));
    primitives.fields[7]=MakeField<Primitives,std::int64_t>("i64",offsetof(Primitives,i64));
    primitives.fields[8]=MakeField<Primitives,std::uint64_t>("u64",offsetof(Primitives,u64));
    primitives.fields[9]=MakeField<Primitives,float>("f32",offsetof(Primitives,f32));
    primitives.fields[10]=MakeField<Primitives,double>("f64",offsetof(Primitives,f64));
    assert(ValidateTypeDescriptor(primitives));
    auto valid=TypeDescriptorTraits<TestData>::Get();assert(ValidateTypeDescriptor(valid));
    for(int i=0;i<11;++i){auto d=valid;
        switch(i){case 0:d.fields[0].offset=UINT32_MAX;break;case 1:d.fields[3].array_count=UINT32_MAX;break;
        case 2:d.fields[1].element_size=8;break;case 3:d.fields[0].name[0]=0;break;case 4:std::strcpy(d.fields[1].name,"sequence");break;
        case 5:d.field_count=129;break;case 6:d.fields[0].kind=static_cast<FieldValueKind>(99);break;
        case 7:d.type_id++;break;case 8:d.payload_alignment=3;break;case 9:d.fields[1].offset=0;break;case 10:d.fields[1].array_count=0;break;}
        assert(!ValidateTypeDescriptor(d));}
    const std::string base="/r3_"+std::to_string(getpid());int commands[2],reply[2];assert(pipe(commands)==0&&pipe(reply)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){close(commands[1]);close(reply[0]);unsetenv(SUPERVISION_FD_ENV);Element e(base);e.command=commands[0];e.reply=reply[1];ProcessRuntime r;r.SetLoopFrequency(500);_exit(r.Run(e)?1:0);}
    close(commands[0]);close(reply[1]);auto runtime=Runtime(child);Check(runtime,base);
    IntrospectionClient c;std::vector<TypeDescriptor> types;assert(c.ListTypes(runtime,types)==0&&types.size()==2);
    std::cout<<"PASS stable ID, descriptor discovery, dedup, undefined/bad/conflict isolation and flattened fields\n";
    const auto name=detail::TypeRegistryName(child,runtime.process_start_ticks);int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);
    detail::ChannelLayout layout;assert(detail::ComputeChannelLayout(sizeof(Payload),alignof(Payload),1,layout));
    struct stat st{};assert(fstat(fd,&st)==0&&static_cast<std::size_t>(st.st_size)==layout.length);
    auto* raw=static_cast<unsigned char*>(mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(raw!=MAP_FAILED);close(fd);
    auto slot=[&](unsigned i)->Slot&{return *reinterpret_cast<Slot*>(raw+layout.slots+i*layout.stride);};
    int ready[2];assert(pipe(ready)==0);pid_t observer=fork();assert(observer>=0);
    if(!observer){close(ready[0]);assert(c.ListTypes(runtime,types)==0);for(unsigned i=0;i<3;++i)slot(i).users.fetch_add(1);Send(ready[1],1);for(;;)pause();}
    close(ready[1]);assert(Receive(ready[0])==1);close(ready[0]);
    Send(commands[1],1);assert(Receive(reply[0])==1);
    std::vector<EndpointInfo> endpoints;assert(c.ListEndpoints(runtime,endpoints)==0);bool extra=false;
    for(const auto& e:endpoints)if(e.name==base+"_extra"){extra=true;assert(!e.type_id);}assert(extra);
    assert(kill(observer,SIGKILL)==0);int code;assert(waitpid(observer,&code,0)==observer&&WIFSIGNALED(code));
    for(unsigned i=0;i<3;++i)slot(i).users.fetch_sub(1);assert(munmap(raw,layout.length)==0);
    Send(commands[1],2);assert(Receive(reply[0])==2);assert(c.ListTypes(runtime,types)==0&&types.size()==3);
    Send(commands[1],3);assert(Receive(reply[0])==3);assert(c.ListTypes(runtime,types)==0&&types.size()==64);
    assert(c.ListEndpoints(runtime,endpoints)==0);bool capacity=false;
    for(const auto& e:endpoints)if(e.name==base+"_full"){capacity=true;assert(!e.type_id);}assert(capacity);
    Check(runtime,base);assert(kill(child,SIGTERM)==0);assert(waitpid(child,&code,0)==child&&WIFEXITED(code)&&WEXITSTATUS(code)==0);
    fd=shm_open(name.c_str(),O_RDONLY,0);assert(fd<0&&errno==ENOENT);close(commands[1]);close(reply[0]);
    std::cout<<"PASS slow/killed type observer, capacity isolation and cleanup\n";
    // Client must reject malformed stored descriptors and preserve caller output.
    RuntimeInfo self{};self.pid=getpid();assert(detail::ReadProcessIdentity(self.pid,self.process_start_ticks));
    SharedChannel<detail::TypeRegistrySnapshot> fixture;auto object=detail::TypeRegistryName(self.pid,self.process_start_ticks);
    assert(fixture.Create(object)==0);auto snapshot=std::make_unique<detail::TypeRegistrySnapshot>();snapshot->pid=self.pid;snapshot->process_start_ticks=self.process_start_ticks;
    snapshot->type_count=1;snapshot->types[0]=valid;snapshot->types[0].fields[0].offset=UINT32_MAX;
    assert(fixture.TryPublishSnapshot(*snapshot)==0);auto count=types.size();assert(c.ListTypes(self,types)==-EPROTO&&types.size()==count);
    snapshot->types[0]=valid;snapshot->type_count=65;assert(fixture.TryPublishSnapshot(*snapshot)==0);assert(c.ListTypes(self,types)==-EPROTO);
    assert(fixture.Close()==0&&fixture.Unlink()==0);
    char exe[4096]{};assert(readlink("/proc/self/exe",exe,sizeof(exe)-1)>0);
    int notice[2];assert(pipe(notice)==0);pid_t supervisor=fork();assert(supervisor>=0);
    if(!supervisor){close(notice[0]);bringup::Bringup app;int result=app.Setup("r3_test_app",{{"typed_child",exe,{"--child",base}}});Send(notice[1],result);if(result)_exit(1);_exit(app.Run());}
    close(notice[1]);assert(Receive(notice[0])==0);close(notice[0]);SupervisorInfo sup{};
    Until([&]{std::vector<SupervisorInfo> all;assert(c.ListSupervisors(all)==0);for(const auto& s:all)if(s.pid==supervisor&&s.application_state==ApplicationState::RUNNING){sup=s;return true;}return false;});
    assert(sup.element_count==1);auto managed=Runtime(sup.elements[0].pid);assert(managed.process_start_ticks==sup.elements[0].process_start_ticks);Check(managed,base);
    assert(kill(supervisor,SIGTERM)==0);assert(waitpid(supervisor,&code,0)==supervisor&&WIFEXITED(code)&&WEXITSTATUS(code)==0);
    std::cout<<"PASS client validation and Supervisor -> Runtime -> Endpoint -> Type\nR3 type descriptor PASS\n";
}
