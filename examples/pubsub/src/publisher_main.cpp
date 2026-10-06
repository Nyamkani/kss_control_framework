#include "pubsub/message.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <cerrno>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <time.h>

class PublisherElement : public kcf::ProcessElement
{
public:
    explicit PublisherElement(kcf::ExecutionMode mode) : mode_(mode) {}
    int Setup() override
    {
        const int result=publisher_.Create(pubsub::TOPIC);
        owned_=result==0;
        if(!result) std::cout<<"[Publisher] mode="<<(mode_==kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")<<std::endl;
        return result;
    }
    int Loop() override
    {
        if(message_.count==std::numeric_limits<std::int32_t>::max()) return -EOVERFLOW;
        timespec now{};
        if(clock_gettime(CLOCK_MONOTONIC,&now)!=0) return -errno;
        message_.timestamp_us=static_cast<std::uint64_t>(now.tv_sec)*1000000u+now.tv_nsec/1000u;
        ++message_.sequence;
        ++message_.count;
        message_.value=message_.count*0.5f;
        message_.active=true;
        const int result=publisher_.Publish(message_);
        if(result==-EAGAIN) return 0; // latest-value Topic may skip a busy sample
        if(result) return result;
        std::cout<<"[Publisher] seq="<<message_.sequence<<" count="<<message_.count<<" value="<<message_.value<<std::endl;
        return 0;
    }
    void Shutdown() override
    {
        if(!owned_) return;
        const int closed=publisher_.Close();
        const int unlinked=publisher_.Unlink();
        owned_=false;
        if(closed || unlinked) throw std::runtime_error("Publisher cleanup failed");
        std::cout<<"[Publisher] Shutdown complete"<<std::endl;
    }
private:
    kcf::ExecutionMode mode_;
    kcf::Publisher<pubsub::PubSubMessage> publisher_;
    pubsub::PubSubMessage message_{};
    bool owned_{false};
};
int main()
{
    PublisherElement element(kcf::DetectLaunchExecutionMode());
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(1.0);
    return runtime.Run(element);
}
