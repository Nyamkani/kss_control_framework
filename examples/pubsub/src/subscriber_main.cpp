#include "pubsub/message.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <cerrno>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

class SubscriberElement : public kcf::ProcessElement
{
public:
    explicit SubscriberElement(kcf::ExecutionMode mode) : mode_(mode) {}
    int Setup() override
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        int result;
        do
        {
            result=subscriber_.Create(pubsub::TOPIC,[this](const pubsub::PubSubMessage& message)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_=message;
                ++generation_;
            });
            if(result!=-ENOENT && result!=-EAGAIN) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while(std::chrono::steady_clock::now()<deadline);
        opened_=result==0;
        if(!result) std::cout<<"[Subscriber] mode="<<(mode_==kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")<<std::endl;
        return result;
    }
    int Loop() override
    {
        pubsub::PubSubMessage message{};
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message=latest_;
            generation=generation_;
        }
        if(generation!=observed_)
        {
            observed_=generation;
            std::cout<<"[Subscriber] seq="<<message.sequence<<" count="<<message.count<<" value="<<message.value<<std::endl;
        }
        return 0;
    }
    void Shutdown() override
    {
        if(!opened_) return;
        const int result=subscriber_.Close();
        opened_=false;
        if(result) throw std::runtime_error("Subscriber cleanup failed");
        std::cout<<"[Subscriber] Shutdown complete"<<std::endl;
    }
private:
    kcf::ExecutionMode mode_;
    std::mutex mutex_;
    pubsub::PubSubMessage latest_{};
    std::uint64_t generation_{0}, observed_{0};
    kcf::Subscriber<pubsub::PubSubMessage> subscriber_;
    bool opened_{false};
};
int main()
{
    SubscriberElement element(kcf::DetectLaunchExecutionMode());
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(10.0);
    return runtime.Run(element);
}
