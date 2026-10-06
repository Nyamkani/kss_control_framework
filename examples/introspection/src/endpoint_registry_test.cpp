#ifdef NDEBUG
#undef NDEBUG
#endif
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/parameter/parameter.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Test-only injection at the real pthread boundary, never a production hook.
thread_local int fail_lock_countdown = 0;
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (fail_lock_countdown && --fail_lock_countdown == 0) return ENOTRECOVERABLE;
    return __real_pthread_mutex_lock(mutex);
}
namespace {
void Send(int fd, int n) { assert(write(fd, &n, sizeof(n)) == sizeof(n)); }
int Receive(int fd) {
    pollfd event{fd,POLLIN,0}; assert(poll(&event,1,5000)==1);
    int n; assert(read(fd,&n,sizeof(n))==sizeof(n)); return n;
}
struct Message { std::uint64_t value; };
struct Element : kcf::ProcessElement {
    int command, reply;
    std::string base;
    kcf::Publisher<Message> publisher;
    kcf::Subscriber<Message> first, second;
    kcf::Parameter<int> owner, client;
    std::vector<std::unique_ptr<kcf::Parameter<int>>> extra;
    Element(int c,int r,std::string b):command(c),reply(r),base(std::move(b)){}
    int Setup() override {
        assert(publisher.Create(base+"_out")==0);
        assert(first.Create(base+"_in",[](const Message&){})==0);
        assert(second.Create(base+"_in",[](const Message&){})==0);
        assert(owner.Create(base+"_parameter",3)==0);
        assert(client.Open(base+"_parameter")==0);
        // Failed creates must not disturb registrations of already open objects.
        assert(publisher.Create(base+"_out")==-EBUSY);
        assert(second.Create(base+"_in",[](const Message&){})==-EBUSY);
        assert(client.Open(base+"_parameter")==-EBUSY);
        Send(reply,1); return 0;
    }
    int Loop() override {
        pollfd event{command,POLLIN,0};
        if (poll(&event,1,0)!=1) return 0;
        const int action=Receive(command);
        if(action==2) { assert(first.Close()==0); }
        if(action==3) {
            for(int i=0;i<140;++i) {
                auto p=std::make_unique<kcf::Parameter<int>>();
                assert(p->Create(base+"_extra_"+std::to_string(i),i)==0);
                assert(p->Set(i+1)==0); int value=0; assert(p->Get(value)==0 && value==i+1);
                extra.push_back(std::move(p));
            }
            kcf::Publisher<Message> overflow;
            assert(overflow.Create(base+"_overflow")==0);
            assert(overflow.Publish({42})==0);
            assert(overflow.Close()==0); assert(overflow.Unlink()==0);
        }
        if(action==4) {
            assert(publisher.Publish({77})==0);
            assert(client.Set(9)==0); int value=0; assert(owner.Get(value)==0 && value==9);
        }
        if(action==5) {
            // The real worker succeeds even when registry Set is unrecoverable.
            fail_lock_countdown=2; // registry local mutex then SharedParameter mutex
            kcf::Subscriber<Message> unobserved;
            assert(unobserved.Create(base+"_in",[](const Message&){})==0);
            assert(fail_lock_countdown==0);
            assert(unobserved.Close()==0);
        }
        if(action==6) {
            for(auto& p:extra) { assert(p->Close()==0); assert(p->Unlink()==0); }
            extra.clear();
        }
        Send(reply,action); return 0;
    }
    void Shutdown() override {
        assert(first.Close()==0); assert(second.Close()==0);
        assert(client.Close()==0); assert(owner.Close()==0); assert(owner.Unlink()==0);
        for(auto& p:extra) { assert(p->Close()==0); assert(p->Unlink()==0); }
        assert(publisher.Close()==0); assert(publisher.Unlink()==0);
    }
};
kcf::RuntimeInfo Runtime(pid_t pid) {
    kcf::IntrospectionClient client; std::vector<kcf::RuntimeInfo> values;
    for(int i=0;i<1000;++i) {
        assert(client.ListRuntimes(values)==0);
        for(const auto& v:values) if(v.pid==pid && v.state==kcf::ProcessState::RUNNING) return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(false); return {};
}
std::vector<kcf::EndpointInfo> Endpoints(const kcf::RuntimeInfo& runtime) {
    kcf::IntrospectionClient c; std::vector<kcf::EndpointInfo> v;
    assert(c.ListEndpoints(runtime,v)==0); return v;
}
void Missing(const std::string& name) {
    int fd=shm_open(name.c_str(),O_RDONLY,0); assert(fd<0 && errno==ENOENT);
}
}
int main() {
    alarm(60);
    using namespace kcf;
    const std::string base="/r2a_"+std::to_string(getpid());
    // No ProcessRuntime: IPC still works and does not create an endpoint registry.
    Publisher<Message> helper; assert(helper.Create(base+"_in")==0);
    std::uint64_t parent_ticks=0; assert(detail::ReadProcessIdentity(getpid(),parent_ticks));
    Missing(detail::EndpointRegistryName(getpid(),parent_ticks));
    for(bool unavailable : {false,true}) {
        int commands[2],replies[2]; assert(pipe(commands)==0 && pipe(replies)==0);
        pid_t child=fork(); assert(child>=0);
        if(child==0) {
            alarm(20); close(commands[1]); close(replies[0]);
            unsetenv(SUPERVISION_FD_ENV);
            assert(Receive(commands[0])==0);
            ProcessRuntime runtime; runtime.SetLoopFrequency(500);
            Element element(commands[0],replies[1],base);
            _exit(runtime.Run(element)==0?0:1);
        }
        close(commands[0]); close(replies[1]);
        std::uint64_t ticks=0; assert(detail::ReadProcessIdentity(child,ticks));
        const auto name=detail::EndpointRegistryName(child,ticks);
        SharedParameter<detail::EndpointRegistrySnapshot> collision;
        if(unavailable) assert(collision.Create(name,{})==0);
        Send(commands[1],0); assert(Receive(replies[0])==1);
        const auto runtime=Runtime(child);
        assert(runtime.execution_mode==ExecutionMode::STANDALONE);
        IntrospectionClient client;
        if(unavailable) {
            std::vector<EndpointInfo> unchanged(1);
            assert(client.ListEndpoints(runtime,unchanged)==-EPROTO && unchanged.size()==1);
            Send(commands[1],4); assert(Receive(replies[0])==4);
            assert(collision.Close()==0); assert(collision.Unlink()==0);
            std::cout<<"PASS unavailable registry: real Parameter/Topic remain operational\n";
        } else {
            auto values=Endpoints(runtime); assert(values.size()==5);
            unsigned publishers=0,subscribers=0,owners=0,clients=0;
            std::vector<std::uint64_t> subscriber_ids;
            for(const auto& v:values) {
                assert(v.registration_id && v.diagnostic_type_name[0]);
                assert(v.payload_size==(v.kind==EndpointKind::TOPIC?sizeof(Message):sizeof(int)));
                if(v.role==EndpointRole::PUBLISHER) { ++publishers; assert(v.name==base+"_out"); }
                if(v.role==EndpointRole::SUBSCRIBER) { ++subscribers; subscriber_ids.push_back(v.registration_id); assert(v.name==base+"_in"); }
                if(v.role==EndpointRole::PARAMETER_OWNER) ++owners;
                if(v.role==EndpointRole::PARAMETER_CLIENT) ++clients;
            }
            assert(publishers==1 && subscribers==2 && owners==1 && clients==1);
            assert(subscriber_ids[0]!=subscriber_ids[1]);
            SharedParameter<detail::EndpointRegistrySnapshot> probe; assert(probe.Open(name)==0);
            detail::EndpointRegistrySnapshot before{},after{};
            assert(probe.Get(before)==0);
            Send(commands[1],4); assert(Receive(replies[0])==4);
            assert(probe.Get(after)==0 && after.revision==before.revision);
            Send(commands[1],2); assert(Receive(replies[0])==2);
            values=Endpoints(runtime); assert(values.size()==4);
            unsigned remaining=0;
            for(const auto& v:values) if(v.role==EndpointRole::SUBSCRIBER) {
                ++remaining; assert(v.registration_id==subscriber_ids[0] || v.registration_id==subscriber_ids[1]);
            }
            assert(remaining==1); assert(probe.Get(after)==0 && after.revision==before.revision+1);
            Send(commands[1],3); assert(Receive(replies[0])==3);
            assert(Endpoints(runtime).size()==detail::MAX_ENDPOINTS);
            std::cout<<"PASS standalone roles, duplicate close, capacity and unchanged data-path revision\n";
            auto stale=runtime; ++stale.process_start_ticks;
            assert(client.ListEndpoints(stale,values)==-ENOENT);
            // Logical corruption must be rejected without altering caller output.
            assert(probe.Get(before)==0); after=before; after.endpoint_count=detail::MAX_ENDPOINTS+1;
            assert(probe.Set(after)==0);
            const auto size=values.size(); assert(client.ListEndpoints(runtime,values)==-EPROTO && values.size()==size);
            assert(probe.Set(before)==0); assert(probe.Close()==0);
            int notice[2]; assert(pipe(notice)==0);
            pid_t observer=fork(); assert(observer>=0);
            if(observer==0) { close(notice[0]); assert(Endpoints(runtime).size()==128); Send(notice[1],1); for(;;) pause(); }
            close(notice[1]); assert(Receive(notice[0])==1); assert(kill(observer,SIGKILL)==0);
            int status; assert(waitpid(observer,&status,0)==observer && WIFSIGNALED(status)); close(notice[0]);
            assert(Runtime(child).state==ProcessState::RUNNING);
            Send(commands[1],4); assert(Receive(replies[0])==4);
            std::cout<<"PASS observer SIGKILL and corruption validation\n";
            Send(commands[1],6); assert(Receive(replies[0])==6);
            Send(commands[1],5); assert(Receive(replies[0])==5);
            assert(client.ListEndpoints(runtime,values)==-ENOENT);
            Send(commands[1],4); assert(Receive(replies[0])==4);
            std::cout<<"PASS unrecoverable registry Set: Subscriber succeeds, data paths survive\n";
        }
        assert(kill(child,SIGTERM)==0); int status=0;
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        Missing(name); Missing(detail::RuntimeRegistryName(child,ticks));
        std::vector<EndpointInfo> values; assert(client.ListEndpoints(runtime,values)==-ENOENT);
        close(commands[1]);close(replies[0]);
    }
    assert(helper.Close()==0); assert(helper.Unlink()==0);
    std::cout<<"R2A endpoint registry PASS\n";
}
