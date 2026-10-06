#ifdef NDEBUG
#undef NDEBUG
#endif
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using State = kcf::ProcessState;
class Element : public kcf::ProcessElement
{
public:
    kcf::ProcessRuntime& runtime;
    int peer{-1}, setups{0}, loops{0}, shutdowns{0}, resource[2]{-1,-1};
    int setup_error{0}, loop_error{0}, expected{0};
    bool setup_throw{false}, loop_throw{false}, shutdown_throw{false}, sigterm{false}, loss{false};
    explicit Element(kcf::ProcessRuntime& r) : runtime(r) {}
    int Setup() override
    {
        ++setups;
        assert(pipe(resource)==0); // partial initialization must also be cleaned
        if(setup_throw) throw std::runtime_error("Setup");
        return setup_error;
    }
    int Loop() override
    {
        ++loops;
        if(loops<3) return 0;
        if(loss)
        {
            close(peer); peer=-1;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(runtime.GetState()!=State::ERROR)
            {
                assert(std::chrono::steady_clock::now()<deadline);
                std::this_thread::yield();
            }
        }
        if(loop_throw) throw std::runtime_error("Loop");
        if(loop_error) return loop_error;
        if(sigterm) assert(raise(SIGTERM)==0);
        else runtime.RequestStop();
        return 0;
    }
    void Shutdown() override
    {
        assert(++shutdowns==1);
        assert(runtime.GetState()==(expected ? State::ERROR : State::STOPPING));
        assert(!runtime.IsRunning());
        close(resource[0]); close(resource[1]); resource[0]=resource[1]=-1;
        if(peer>=0)
        {
            kcf::RuntimeStatusRequest request{}; request.request_id=1;
            assert(send(peer,&request,sizeof(request),MSG_NOSIGNAL)==sizeof(request));
            pollfd event{peer,POLLIN,0}; assert(poll(&event,1,1000)==1);
            kcf::RuntimeStatusResponse response{};
            assert(recv(peer,&response,sizeof(response),0)==sizeof(response));
            assert(response.request_id==1 && response.runtime_error==expected);
            assert(response.state==(expected ? State::ERROR : State::STOPPING));
            const auto heartbeat=loops==0 ? 0 : (loop_error || loop_throw) ? 2 : 3;
            assert(response.loop_heartbeat==static_cast<std::uint64_t>(heartbeat));
        }
        if(shutdown_throw) throw std::runtime_error("Shutdown");
    }
};

int main()
{
    alarm(30);
    assert(unsetenv(kcf::SUPERVISION_FD_ENV)==0);
    assert(kcf::DetectLaunchExecutionMode()==kcf::ExecutionMode::STANDALONE);
    assert(setenv(kcf::SUPERVISION_FD_ENV,"",1)==0);
    assert(kcf::DetectLaunchExecutionMode()==kcf::ExecutionMode::SUPERVISED);
    assert(unsetenv(kcf::SUPERVISION_FD_ENV)==0);
    auto fds=[] {return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});};
    const auto baseline=fds();
    for(int mode=0;mode<11;++mode)
    {
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(1000);
        Element element(runtime);
        if(mode==1) element.setup_error=element.expected=-EINVAL;
        if(mode==2) {element.setup_throw=true;element.expected=-EFAULT;}
        if(mode==3) element.loop_error=element.expected=-EIO;
        if(mode==4) {element.loop_throw=true;element.expected=-EFAULT;}
        if(mode==5) element.sigterm=true;
        if(mode==6) {element.setup_error=element.expected=7;element.shutdown_throw=true;}
        if(mode==7) {element.loop_error=element.expected=9;element.shutdown_throw=true;}
        if(mode==8) {element.loss=true;element.loop_error=-EIO;element.expected=-ECONNRESET;}
        if(mode==9) element.shutdown_throw=true;
        if(mode==10) {element.setup_throw=element.shutdown_throw=true;element.expected=-EFAULT;}
        int sockets[2]; assert(socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets)==0);
        element.peer=sockets[0];
        assert(setenv(kcf::SUPERVISION_FD_ENV,std::to_string(sockets[1]).c_str(),1)==0);
        assert(kcf::DetectLaunchExecutionMode()==kcf::ExecutionMode::SUPERVISED);
        const int result=runtime.Run(element);
        assert(std::getenv(kcf::SUPERVISION_FD_ENV)==nullptr);
        const int expected=mode==9 ? -EFAULT : element.expected;
        assert(result==expected && runtime.GetState()==(expected ? State::ERROR : State::STOPPED));
        assert(element.setups==1 && element.shutdowns==1);
        assert(element.loops==((element.setup_error || element.setup_throw) ? 0 : 3));
        assert(element.resource[0]==-1 && element.resource[1]==-1);
        if(element.peer>=0) close(element.peer);
        assert(fds()==baseline);
        std::cout<<"Lifecycle case="<<mode<<" result="<<result<<" PASS"<<std::endl;
    }
    {
        kcf::ProcessRuntime runtime; Element element(runtime);
        assert(setenv(kcf::SUPERVISION_FD_ENV,"invalid",1)==0);
        assert(kcf::DetectLaunchExecutionMode()==kcf::ExecutionMode::SUPERVISED);
        assert(runtime.Run(element)==-EINVAL && runtime.GetState()==State::ERROR);
        assert(element.setups==0 && element.loops==0 && element.shutdowns==0);
        assert(fds()==baseline);
    }
    std::cout<<"ExecutionMode / Runtime lifecycle/cleanup/error precedence/heartbeat/FD PASS"<<std::endl;
}
