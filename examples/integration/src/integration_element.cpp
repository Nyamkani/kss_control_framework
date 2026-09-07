#include "integration/integration_element.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace integration
{
namespace
{
void Check(int result)
{
    if (result != 0) throw std::runtime_error("KCF error " + std::to_string(result));
}
}
int IntegrationElement::Setup()
{
    try
    {
        Check(state_pub_.Create(STATE_TOPIC));
        state_owned_ = true;
        std::cout << "[IntegrationBackend] waiting for command owner" << std::endl;
        // The separate client owns Command. Publish State first so its initial
        // Subscriber can open, then wait for its Publisher (bounded startup).
        int opened = -ENOENT;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            opened = command_sub_.Create(COMMAND_TOPIC, [this](const IntegrationCommand& command)
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.target = command.target;
            });
            if (opened != -ENOENT && opened != -EAGAIN) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        Check(opened);
        Check(service_.Create(SERVICE_PORT));
        Check(service_.Register<AddRequest, AddResponse>(SERVICE_ADD,
            [](const AddRequest& request, AddResponse& response)
            {
                const auto sum = std::int64_t(request.a) + request.b;
                if (sum < std::numeric_limits<std::int32_t>::min() ||
                    sum > std::numeric_limits<std::int32_t>::max()) throw std::overflow_error("Add");
                response.sum = static_cast<std::int32_t>(sum);
            }));
        Check(service_.Register<SetValueRequest, SetValueResponse>(SERVICE_SET_VALUE,
            [this](const SetValueRequest& request, SetValueResponse& response)
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_.service_value = request.value;
                response.value = request.value;
            }));
        Check(gain_.Create(GAIN_PARAMETER, 1.0, [this](const double& value)
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_.gain = value; // State reflects the Parameter callback, not polling.
        }));
        gain_owned_ = true;
        Check(action_.Create(service_, ACTION_IDS, ACTION_TOPIC,
            [this](std::uint64_t, const CountGoal& goal)
            {
                if (!goal.target_count || goal.target_count > 10000) return -EINVAL;
                std::lock_guard<std::mutex> lock(state_mutex_);
                goal_target_ = goal.target_count;
                state_.action_count = 0;
                return 0;
            }));
        action_created_ = true;
        Check(service_.Start());
        Check(state_timer_.Create(50.0, [this]
        {
            IntegrationState snapshot;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ++state_.sequence;
                snapshot = state_;
            }
            const int result = state_pub_.Publish(snapshot); // outside application lock
            if (result != 0 && result != -EAGAIN) async_error_.store(result);
        }));
        std::cout << "[IntegrationBackend] ready port=22100 loop_hz=100 state_hz=50" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[IntegrationBackend] Setup failed: " << error.what() << std::endl;
        // Runtime does not call Shutdown after failed Setup.
        try { Shutdown(); } catch (...) {}
        return -1;
    }
}

void IntegrationElement::Loop()
{
    Check(async_error_.load());
    const auto state = action_.GetState();
    if (state != kcf::ActionState::ACCEPTED && state != kcf::ActionState::RUNNING) return;
    const auto id = action_.GetGoalId();
    if (id != executing_)
    {
        executing_ = id;
        count_ = 0;
        cancel_pending_ = false;
    }
    if (action_.IsCancelRequested(id))
    {
        // Simulate a 100ms safe-stop FSM without blocking Loop or any worker.
        if (!cancel_pending_)
        {
            cancel_pending_ = true;
            cancel_ready_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        }
        if (std::chrono::steady_clock::now() >= cancel_ready_)
            Check(action_.Canceled(id, {count_, -ECANCELED}));
        return;
    }
    if (state == kcf::ActionState::ACCEPTED)
    {
        const int result = action_.Start(id);
        if (result == -EINVAL && action_.IsCancelRequested(id)) return;
        Check(result);
        return;
    }
    std::uint32_t target;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        target = goal_target_;
        state_.action_count = ++count_;
    }
    Check(action_.PublishFeedback(id, {count_, float(count_) / target}));
    if (count_ >= target) Check(action_.Succeed(id, {count_, 0}));
}

void IntegrationElement::Shutdown()
{
    // Loop has stopped. Join all producers/callbacks before releasing their data.
    state_timer_.Stop();
    service_.Stop();
    int error = 0;
    const auto record = [&](int result) { if (result && !error) error = result; };
    record(command_sub_.Close());
    record(gain_.Close());
    if (gain_owned_) { record(gain_.Unlink()); gain_owned_ = false; }
    if (action_created_) { record(action_.Unlink()); action_created_ = false; }
    action_.Close();
    record(state_pub_.Close());
    if (state_owned_) { record(state_pub_.Unlink()); state_owned_ = false; }
    std::cout << "[IntegrationBackend] Shutdown complete error=" << error << std::endl;
    Check(error);
}
} // namespace integration
