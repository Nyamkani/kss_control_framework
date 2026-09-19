#ifdef NDEBUG
#undef NDEBUG
#endif
#include "kcf/process/process.hpp"
#include "kcf/system/system_status_channel.hpp"
#include "kcf/system/system_status.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using Clock=std::chrono::steady_clock;
static std::size_t Fds(){return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{});}
static kcf::RuntimeStatusResponse Query(kcf::Process& process,std::uint64_t id)
{
    assert(process.RequestRuntimeStatus(id)==0);
    const auto end=Clock::now()+std::chrono::seconds(2);
    kcf::RuntimeStatusResponse response{};
    for(;;)
    {
        const int result=process.ReceiveRuntimeStatus(response);
        if(!result)return response;
        assert(result==-EAGAIN||result==-EINTR);
        assert(Clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main(int argc,char**argv)
{
    if((argc==2 || argc==4) && std::string(argv[1])=="--recover-system-status")
    {
        if(argc==4){const auto scope=kcf::detail::SystemStatusScope(std::stoi(argv[2]),std::stoull(argv[3]));
            assert(!scope.empty());assert(setenv(kcf::detail::SYSTEM_STATUS_SCOPE_ENV,scope.c_str(),1)==0);}
        kcf::SystemStatusPublisher owner;
        assert(owner.Create(kcf::SystemStatus{})==0);
        assert(owner.Close()==0);assert(owner.Unlink()==0);return 0;
    }
    if((argc==2 || argc==4) && std::string(argv[1])=="--read-system-status")
    {
        kcf::SystemStatusSubscriber peer;
        assert((argc==4?peer.OpenForApplication(std::stoi(argv[2]),std::stoull(argv[3])):peer.Open())==0);
        kcf::SystemStatus status{};
        assert(peer.ReadCurrent(status)==0);
        std::cout<<"state="<<int(status.state)<<" failure_kind="<<int(status.failure_kind)
                 <<" origin="<<status.error_element<<" pid="<<status.failed_pid
                 <<" runtime_error="<<status.runtime_error<<" termination="<<int(status.termination_kind)<<std::endl;
        assert(peer.Close()==0);return 0;
    }
    if(argc==3 && std::string(argv[1])=="--protocol-peer")
    {
        assert(std::string(argv[2])=="spaces ; $literal");
        assert(std::string(getenv("KCF_TEST_ENV"))=="preserved");
        const int fd=std::stoi(getenv(kcf::SUPERVISION_FD_ENV));
        pollfd event{fd,POLLIN,0};assert(poll(&event,1,2000)==1);
        kcf::RuntimeStatusRequest request{};assert(recv(fd,&request,sizeof(request),0)==sizeof(request));
        kcf::RuntimeStatusResponse good{};good.pid=getpid();good.state=kcf::ProcessState::RUNNING;good.request_id=request.request_id;
        auto bad=good;bad.magic=0;assert(send(fd,&bad,sizeof(bad),MSG_NOSIGNAL)==sizeof(bad));
        bad=good;bad.pid++;assert(send(fd,&bad,sizeof(bad),MSG_NOSIGNAL)==sizeof(bad));
        bad=good;bad.request_id++;assert(send(fd,&bad,sizeof(bad),MSG_NOSIGNAL)==sizeof(bad));
        assert(send(fd,&good,sizeof(good),MSG_NOSIGNAL)==sizeof(good));
        return 7;
    }
    assert(argc==2);alarm(25);const auto baseline=Fds();
    for(int iteration=0;iteration<10;++iteration)
    {
        kcf::Process process;assert(process.Start(argv[1],{"--setup-delay-ms","100"})==0);
        assert(fcntl(process.GetSupervisionFd(),F_GETFD)&FD_CLOEXEC);
        auto starting=Query(process,1);assert(starting.state==kcf::ProcessState::STARTING&&starting.loop_heartbeat==0);
        std::this_thread::sleep_for(std::chrono::milliseconds(130));
        auto running=Query(process,2);assert(running.state==kcf::ProcessState::RUNNING&&running.loop_heartbeat>0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto next=Query(process,3);assert(next.loop_heartbeat>running.loop_heartbeat&&next.runtime_error==0);
        // Invalid request is discarded by Runtime, with no application involvement.
        kcf::RuntimeStatusRequest bad{};bad.magic=0;assert(send(process.GetSupervisionFd(),&bad,sizeof(bad),MSG_NOSIGNAL)==sizeof(bad));
        assert(Query(process,4).state==kcf::ProcessState::RUNNING);
        assert(process.RequestStop()==0);assert(process.Wait()==0);assert(process.GetSupervisionFd()==-1);
        assert(process.GetExitInfo().exited_normally&&process.GetExitInfo().exit_code==0);
        assert(Fds()==baseline);
    }
    {
        setenv("KCF_TEST_ENV","preserved",1);
        kcf::Process peer;assert(peer.Start(argv[0],{"--protocol-peer","spaces ; $literal"})==0);
        assert(peer.RequestRuntimeStatus(55)==0);assert(peer.RequestRuntimeStatus(56)==-EBUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        kcf::RuntimeStatusResponse r{};
        assert(peer.ReceiveRuntimeStatus(r)==-EPROTO);assert(peer.ReceiveRuntimeStatus(r)==-EPROTO);
        assert(peer.ReceiveRuntimeStatus(r)==-ESTALE);assert(peer.ReceiveRuntimeStatus(r)==0&&r.request_id==55);
        assert(peer.Wait()==0&&peer.GetExitInfo().exit_code==7);unsetenv("KCF_TEST_ENV");
    }
    // Parent socket loss now terminates the supervised Element itself.
    {
        pid_t child;
        {
            kcf::Process detached;
            assert(detached.Start(argv[1])==0);
            Query(detached,1);
            child=detached.GetPid();
        }
        int status=0;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status)&&WEXITSTATUS(status)==((-ECONNRESET)&255));
    }
    {kcf::Process missing;assert(missing.Start("/missing/kcf")==-ENOENT);assert(missing.GetSupervisionFd()==-1);}
    assert(Fds()==baseline);int status;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
    std::cout<<"Runtime supervision protocol/STARTING/RUNNING/heartbeat/arguments/environment/10 FD lifecycles PASS\n";
}
