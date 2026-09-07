#include "kcf/action/action_client.hpp"
#include "action/test_action.hpp"
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    unsigned port = 22010, target = 20, delay = 0, cancel_ms = 0;
    if (argc > 5) return 1;
    unsigned* fields[]{&port, &target, &delay, &cancel_ms};
    for (int i=1; i<argc; ++i)
    {
        std::string_view text(argv[i]);
        auto r = std::from_chars(text.data(),text.data()+text.size(),*fields[i-1]);
        if (r.ec != std::errc{} || r.ptr != text.data()+text.size()) return 1;
    }
    if (!port || port>65535 || !target || target>100 || delay>3000 || cancel_ms>10000) return 1;
    std::atomic<unsigned> callbacks{0};
    kcf::ActionClient<CountGoal, CountFeedback, CountResult> client;
    int error = client.Create(static_cast<std::uint16_t>(port), COUNT_IDS, COUNT_STATUS,
        [&](const auto& status)
        {
            ++callbacks;
            std::cout << "[ActionClient] goal=" << status.header.goal_id << " state=" << int(status.header.state)
                      << " count=" << status.feedback.current_count << " seq=" << status.header.feedback_sequence
                      << " cancel=" << int(status.header.cancel_requested) << std::endl;
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        });
    if (error) { std::cerr << "Create: " << error << std::endl; return 1; }
    std::uint64_t id = 0;
    if ((error = client.SendGoal({target}, id))) { std::cerr << "Goal: " << error << std::endl; return 1; }
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    bool canceled = false;
    CountResult result{};
    kcf::ActionState state{};
    do
    {
        if (cancel_ms && !canceled && Clock::now()-begin >= std::chrono::milliseconds(cancel_ms))
        {
            error = client.Cancel(id);
            if (error) break;
            canceled = true;
        }
        error = client.GetResult(id, result, &state);
        if (error != -EINPROGRESS) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (Clock::now()-begin < std::chrono::seconds(15));
    const int closed = client.Close();
    if (error || closed) { std::cerr << "Result: " << error << " close=" << closed << std::endl; return 1; }
    std::cout << "[ActionClient] result goal=" << id << " state=" << int(state) << " final_count=" << result.final_count
              << " code=" << result.result_code << " callbacks=" << callbacks << std::endl;
    return canceled ? (state == kcf::ActionState::CANCELED && result.result_code == -ECANCELED ? 0 : 1)
                    : (state == kcf::ActionState::SUCCEEDED && result.final_count == target ? 0 : 1);
}
