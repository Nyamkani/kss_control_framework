#include "integration/integration_types.hpp"
#include "kcf/action/action_client.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/parameter/parameter.hpp"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace integration;
using Clock = std::chrono::steady_clock;
namespace
{
void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
void Check(int result)
{
    if (result) throw std::runtime_error("KCF error " + std::to_string(result));
}
template<typename Open> void OpenWhenReady(Open open)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    int result;
    do
    {
        result = open();
        if (result != -ENOENT && result != -EAGAIN) { Check(result); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < deadline);
    Check(result);
}
struct Observations
{
    std::mutex mutex;
    std::condition_variable changed;
    IntegrationState state{};
    kcf::ActionStatus<CountFeedback> action{};
    std::uint64_t feedback_goal{0};
    unsigned invalid{0};
    template<typename Predicate> void Wait(Predicate predicate)
    {
        std::unique_lock<std::mutex> lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(5), predicate), "Topic observation timeout");
        Require(invalid == 0, "Invalid/nonmonotone snapshot");
    }
};
}
int main()
{
    // Captured state outlives every callback, including on error paths.
    Observations seen;
    kcf::Subscriber<IntegrationState> state_sub;
    kcf::Publisher<IntegrationCommand> command_pub;
    kcf::ServiceClient service;
    kcf::Parameter<double> gain;
    kcf::ActionClient<CountGoal, CountFeedback, CountResult> action;
    bool command_owned = false;
    int outcome = 0;
    try
    {
        OpenWhenReady([&] { return state_sub.Create(STATE_TOPIC, [&](const IntegrationState& state)
        {
            {
                std::lock_guard<std::mutex> lock(seen.mutex);
                if (state.sequence <= seen.state.sequence || state.action_count > 10000 ||
                    (state.gain != 1.0 && state.gain != 2.0 && state.gain != 3.0)) ++seen.invalid;
                seen.state = state;
            }
            seen.changed.notify_all();
        }); });
        Check(command_pub.Create(COMMAND_TOPIC));
        command_owned = true;
        Check(service.Create(SERVICE_PORT));
        OpenWhenReady([&] { return gain.Open(GAIN_PARAMETER); });
        OpenWhenReady([&] { return action.Create(SERVICE_PORT, ACTION_IDS, ACTION_TOPIC,
            [&](const kcf::ActionStatus<CountFeedback>& status)
            {
                {
                    std::lock_guard<std::mutex> lock(seen.mutex);
                    if (status.header.goal_id == seen.action.header.goal_id &&
                        status.header.feedback_sequence < seen.action.header.feedback_sequence) ++seen.invalid;
                    seen.action = status;
                    if (status.header.state == kcf::ActionState::RUNNING && status.header.feedback_sequence > 0)
                        seen.feedback_goal = status.header.goal_id;
                }
                seen.changed.notify_all();
            }); });
        const auto publish = [&](int target)
        {
            int result;
            const auto deadline = Clock::now() + std::chrono::seconds(1);
            do { result = command_pub.Publish({target}); }
            while (result == -EAGAIN && Clock::now() < deadline);
            Check(result);
        };
        publish(100);
        seen.Wait([&] { return seen.state.target == 100; });
        for (int target = 101; target <= 120; ++target) publish(target);
        seen.Wait([&] { return seen.state.target == 120; });
        std::uint64_t first_sequence;
        { std::lock_guard<std::mutex> lock(seen.mutex); first_sequence = seen.state.sequence; }
        const auto start = Clock::now();
        seen.Wait([&] { return seen.state.sequence >= first_sequence + 25; });
        const double hz = 25.0 / std::chrono::duration<double>(Clock::now() - start).count();
        Require(hz > 35 && hz < 65, "State timer frequency");
        std::cout << "[IntegrationClient] Topic + Timer PASS hz=" << hz << std::endl;

        AddResponse add{};
        SetValueResponse set{};
        Check(service.Call(SERVICE_ADD, AddRequest{10, 20}, add));
        Require(add.sum == 30, "Add result");
        Check(service.Call(SERVICE_SET_VALUE, SetValueRequest{123}, set));
        Require(set.value == 123, "SetValue result");
        seen.Wait([&] { return seen.state.service_value == 123; });
        std::cout << "[IntegrationClient] Service multi-ID PASS port=22100" << std::endl;

        double value;
        Check(gain.Get(value)); Require(value == 1.0, "Initial gain");
        Check(gain.Set(2.0)); Check(gain.Get(value)); Require(value == 2.0, "Changed gain");
        seen.Wait([&] { return seen.state.gain == 2.0; });
        std::cout << "[IntegrationClient] Parameter Get/Set/backend callback PASS" << std::endl;

        const auto result = [&](std::uint64_t id, kcf::ActionState expected)
        {
            CountResult result{};
            kcf::ActionState state{};
            int error;
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            do
            {
                error = action.GetResult(id, result, &state);
                if (error != -EINPROGRESS) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (Clock::now() < deadline);
            Check(error); Require(state == expected, "Terminal Action state");
            return result;
        };
        std::uint64_t success_id = 0;
        Check(action.SendGoal({20}, success_id));
        seen.Wait([&] { return seen.feedback_goal == success_id; });
        const auto success = result(success_id, kcf::ActionState::SUCCEEDED);
        Require(success.final_count == 20 && success.result_code == 0, "Count success");
        std::cout << "[IntegrationClient] Action Success PASS count=20" << std::endl;

        std::uint64_t cancel_id = 0;
        Check(action.SendGoal({1000}, cancel_id));
        Require(cancel_id != success_id, "Goal ID reused");
        seen.Wait([&] { return seen.action.header.goal_id == cancel_id &&
            seen.action.header.state == kcf::ActionState::RUNNING && seen.feedback_goal == cancel_id; });
        std::uint64_t before;
        { std::lock_guard<std::mutex> lock(seen.mutex); before = seen.state.sequence; }
        publish(777);
        Check(service.Call(SERVICE_ADD, AddRequest{40, 2}, add)); Require(add.sum == 42, "Concurrent Add");
        Check(service.Call(SERVICE_SET_VALUE, SetValueRequest{456}, set)); Require(set.value == 456, "Concurrent SetValue");
        Check(gain.Set(3.0)); Check(gain.Get(value)); Require(value == 3.0, "Concurrent gain");
        seen.Wait([&] { return seen.state.target == 777 && seen.state.service_value == 456 &&
            seen.state.gain == 3.0 && seen.state.sequence >= before + 5; });
        CountResult pending{};
        Require(action.GetResult(cancel_id, pending) == -EINPROGRESS, "Action must still be running");
        std::cout << "[IntegrationClient] Concurrent Topic/Timer/Service/Parameter while RUNNING PASS" << std::endl;

        Check(action.Cancel(cancel_id));
        Require(action.GetResult(cancel_id, pending) == -EINPROGRESS, "Cancel request must precede completion");
        seen.Wait([&] { return seen.action.header.goal_id == cancel_id && seen.action.header.cancel_requested &&
            seen.action.header.state == kcf::ActionState::RUNNING; });
        const auto canceled = result(cancel_id, kcf::ActionState::CANCELED);
        Require(canceled.final_count < 1000 && canceled.result_code == -ECANCELED, "Cancel result");
        std::cout << "[IntegrationClient] Cancel pending -> CANCELED PASS count=" << canceled.final_count << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "[IntegrationClient] FAIL: " << error.what() << std::endl;
        outcome = 1;
    }
    // Explicit cleanup also runs after a failed scenario; no global resource is
    // removed by a non-owner. Join callbacks before destroying observations.
    const auto close = [&](int error) { if (error) { std::cerr << "Cleanup: " << error << std::endl; outcome = 1; } };
    close(action.Close());
    close(gain.Close());
    service.Close();
    close(state_sub.Close());
    close(command_pub.Close());
    if (command_owned) close(command_pub.Unlink());
    std::cout << "[IntegrationClient] " << (outcome ? "FAIL" : "PASS") << " cleanup complete" << std::endl;
    return outcome;
}
