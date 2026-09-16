#include "action_app/count_action.hpp"
#include "kcf/action/action_server.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace action_app;
class ServerElement : public kcf::ProcessElement
{
public:
    explicit ServerElement(std::uint16_t port) : port_(port) {}
    int Setup() override
    {
        int error = service_.Create(port_);
        if (error) return error;
        error = action_.Create(service_, IDS, StatusName(port_),
            [this](std::uint64_t, const CountGoal& goal) {
                if (!goal.target_count || goal.target_count > 10000) return -EINVAL;
                target_.store(goal.target_count);
                return 0;
            }, [](std::uint64_t) {});
        owned_ = error == 0;
        if (error) return error;
        std::cout << "[ActionServer] state=IDLE port=" << port_ << std::endl;
        return service_.Start();
    }
    int Loop() override
    {
        const auto state = action_.GetState();
        const auto id = action_.GetGoalId();
        if (state != kcf::ActionState::ACCEPTED && state != kcf::ActionState::RUNNING) return 0;
        if (executing_ != id)
        {
            executing_ = id; count_ = 0;
            std::cout << "[ActionServer] accepted goal=" << id << " target=" << target_.load() << std::endl;
        }
        int error = 0;
        if (action_.IsCancelRequested(id))
        {
            error = action_.Canceled(id, {count_, -ECANCELED});
            if (!error) std::cout << "[ActionServer] canceled goal=" << id << " final=" << count_ << std::endl;
        }
        else if (state == kcf::ActionState::ACCEPTED)
        {
            error = action_.Start(id);
            if (!error) std::cout << "[ActionServer] running goal=" << id << std::endl;
        }
        else
        {
            ++count_;
            const float progress = float(count_) / target_.load();
            error = action_.PublishFeedback(id, {count_, progress});
            if (!error) std::cout << "[ActionServer] feedback goal=" << id << " count=" << count_ << " progress=" << progress << std::endl;
            if (!error && count_ >= target_.load())
            {
                error = action_.Succeed(id, {count_, 0});
                if (!error) std::cout << "[ActionServer] succeeded goal=" << id << " final=" << count_ << std::endl;
            }
        }
        // Cancel can race the transition from ACCEPTED to RUNNING.
        return error == -EINVAL && action_.IsCancelRequested(id) ? 0 : error;
    }
    void Shutdown() override
    {
        service_.Stop();
        const int error = owned_ ? action_.Unlink() : 0;
        action_.Close(); owned_ = false;
        if (error) throw std::runtime_error("Action unlink failed");
        std::cout << "[ActionServer] Shutdown complete" << std::endl;
    }
private:
    std::uint16_t port_;
    std::atomic<std::uint32_t> target_{0};
    kcf::ServiceServer service_;
    kcf::ActionServer<CountGoal, CountFeedback, CountResult> action_;
    bool owned_{false};
    std::uint64_t executing_{0};
    std::uint32_t count_{0};
};
int main(int argc, char** argv)
{
    std::uint16_t port;
    if (!ParsePort(argc, argv, port)) return 1;
    std::cout << "[ActionServer] mode=" << (kcf::DetectLaunchExecutionMode() == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised") << std::endl;
    ServerElement element(port);
    kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(10.0);
    return runtime.Run(element);
}
