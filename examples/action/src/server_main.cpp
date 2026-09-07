#include "kcf/action/action_server.hpp"
#include "action/test_action.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <signal.h>
#include <string_view>

namespace
{
volatile sig_atomic_t stop_requested = 0;
void HandleStop(int) { stop_requested = 1; }
}
int main(int argc, char** argv)
{
    std::uint16_t port = 22010;
    if (argc > 2) return 1;
    if (argc == 2)
    {
        std::string_view text(argv[1]);
        auto r = std::from_chars(text.data(), text.data()+text.size(), port);
        if (r.ec != std::errc{} || r.ptr != text.data()+text.size() || !port) return 1;
    }
    struct sigaction sa{}; sa.sa_handler = HandleStop;
    if (sigemptyset(&sa.sa_mask) || sigaction(SIGINT, &sa, nullptr) || sigaction(SIGTERM, &sa, nullptr)) return 1;
    std::atomic<std::uint32_t> target{0}, accepted{0}, cancels{0};
    kcf::ServiceServer service;
    kcf::ActionServer<CountGoal, CountFeedback, CountResult> action;
    if (service.Create(port)) return 1;
    int error = action.Create(service, COUNT_IDS, COUNT_STATUS,
        [&](std::uint64_t, const CountGoal& goal)
        {
            (void)action.GetState(); // callback is outside Action lock
            if (!goal.target_count || goal.target_count > 10000) return -EINVAL;
            target.store(goal.target_count);
            ++accepted;
            return 0;
        }, [&](std::uint64_t) { ++cancels; });
    if (error) { std::cerr << "Action Create: " << error << std::endl; return 1; }
    if (service.Start()) { action.Unlink(); return 1; }
    std::cout << "[ActionServer] ready port=" << port << std::endl;
    std::uint64_t executing = 0;
    std::uint32_t count = 0;
    while (!stop_requested)
    {
        const auto state = action.GetState();
        const auto id = action.GetGoalId();
        if (state == kcf::ActionState::ACCEPTED || state == kcf::ActionState::RUNNING)
        {
            if (executing != id) { executing = id; count = 0; }
            if (action.IsCancelRequested(id)) error = action.Canceled(id, {count, -ECANCELED});
            else if (state == kcf::ActionState::ACCEPTED) error = action.Start(id);
            else
            {
                ++count;
                error = action.PublishFeedback(id, {count, float(count)/target.load()});
                if (!error && count >= target.load()) error = action.Succeed(id, {count, 0});
            }
            // Cancel may arrive between IsCancelRequested and Start.
            if (error && !(error == -EINVAL && action.IsCancelRequested(id))) break;
            error = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    service.Stop();
    const int unlinked = action.Unlink();
    action.Close();
    std::cout << "[ActionServer] stopped goals=" << accepted << " cancels=" << cancels << std::endl;
    return error || unlinked ? 1 : 0;
}
