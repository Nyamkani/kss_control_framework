#include "action_app/count_action.hpp"
#include "kcf/action/action_client.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace action_app;
class ClientElement : public kcf::ProcessElement
{
    using Clock = std::chrono::steady_clock;
public:
    explicit ClientElement(std::uint16_t port) : port_(port) {}
    int Setup() override
    {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        int error;
        do {
            error = client_.Create(port_, IDS, StatusName(port_), [this](const auto& status) {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_ = status;
            });
            if (error != -ENOENT && error != -EAGAIN) return error;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Clock::now() < deadline);
        return error;
    }
    int Loop() override
    {
        if (completed_) return 0;
        if (!id_)
        {
            const int error = client_.SendGoal({scenario_ == 0 ? 10u : 50u}, id_);
            if (error) return Failure(error);
            started_ = Clock::now(); sequence_ = 0; canceled_ = false;
            std::cout << "[ActionClient] sent " << (scenario_ == 0 ? "success" : "cancel") << " goal id=" << id_ << " target=" << (scenario_ == 0 ? 10 : 50) << std::endl;
        }
        kcf::ActionStatus<CountFeedback> snapshot{};
        { std::lock_guard<std::mutex> lock(mutex_); snapshot = latest_; }
        if (snapshot.header.goal_id == id_ && snapshot.header.feedback_sequence > sequence_)
        {
            sequence_ = snapshot.header.feedback_sequence;
            std::cout << "[ActionClient] feedback id=" << id_ << " seq=" << sequence_
                      << " count=" << snapshot.feedback.current_count << " progress=" << snapshot.feedback.progress << std::endl;
        }
        if (scenario_ == 1 && !canceled_ && Clock::now()-started_ >= std::chrono::milliseconds(500))
        {
            const int error = client_.Cancel(id_);
            if (error) return Failure(error);
            canceled_ = true;
            std::cout << "[ActionClient] cancel requested id=" << id_ << std::endl;
        }
        CountResult result{}; kcf::ActionState state{};
        const int error = client_.GetResult(id_, result, &state);
        if (error == -EINPROGRESS)
            return Clock::now()-started_ < std::chrono::seconds(10) ? 0 : Failure(-ETIMEDOUT);
        if (error) return Failure(error);
        const bool valid = scenario_ == 0
            ? state == kcf::ActionState::SUCCEEDED && result.final_count == 10 && result.result_code == 0
            : state == kcf::ActionState::CANCELED && result.final_count < 50 && result.result_code == -ECANCELED;
        if (!valid) return Failure(-EPROTO);
        std::cout << "[ActionClient] result id=" << id_ << " state=" << (scenario_ == 0 ? "SUCCEEDED" : "CANCELED")
                  << " final=" << result.final_count << " code=" << result.result_code << std::endl;
        id_ = 0;
        if (++scenario_ == 2) { completed_ = true; std::cout << "[ActionClient] scenarios complete; idle/RUNNING" << std::endl; }
        return 0;
    }
    void Shutdown() override
    {
        if (client_.Close()) throw std::runtime_error("Action client close failed");
        std::cout << "[ActionClient] Shutdown complete" << std::endl;
    }
private:
    int Failure(int error) { std::cerr << "[ActionClient] error=" << error << std::endl; return error; }
    std::uint16_t port_;
    std::mutex mutex_;
    kcf::ActionStatus<CountFeedback> latest_{};
    kcf::ActionClient<CountGoal, CountFeedback, CountResult> client_;
    std::uint64_t id_{0}, sequence_{0};
    unsigned scenario_{0};
    bool canceled_{false}, completed_{false};
    Clock::time_point started_{};
};
int main(int argc, char** argv)
{
    std::uint16_t port;
    if (!ParsePort(argc, argv, port)) return 1;
    std::cout << "[ActionClient] mode=" << (kcf::DetectLaunchExecutionMode() == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised") << std::endl;
    ClientElement element(port);
    kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(20.0);
    return runtime.Run(element);
}
