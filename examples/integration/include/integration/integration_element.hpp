#pragma once
#include "integration/integration_types.hpp"
#include "kcf/process/process_element.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/timer/timer.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/action/action_server.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

namespace integration
{
class IntegrationElement : public kcf::ProcessElement
{
public:
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
private:
    // Application state; callbacks and Loop share this short critical section.
    std::mutex state_mutex_;
    IntegrationState state_{};
    std::uint32_t goal_target_{0};
    std::atomic<int> async_error_{0};
    kcf::Subscriber<IntegrationCommand> command_sub_;
    kcf::Publisher<IntegrationState> state_pub_;
    kcf::Timer state_timer_;
    kcf::ServiceServer service_;
    kcf::Parameter<double> gain_;
    kcf::ActionServer<CountGoal, CountFeedback, CountResult> action_;
    bool state_owned_{false}, gain_owned_{false}, action_created_{false};
    // Only ProcessRuntime::Loop accesses these FSM fields.
    std::uint64_t executing_{0};
    std::uint32_t count_{0};
    bool cancel_pending_{false};
    std::chrono::steady_clock::time_point cancel_ready_{};
};
} // namespace integration
