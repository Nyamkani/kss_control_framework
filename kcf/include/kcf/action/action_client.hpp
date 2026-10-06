#pragma once
#include "kcf/action/action_types.hpp"
#include "kcf/service/service_client.hpp"
#include "kcf/ipc/subscriber.hpp"
#include <limits>
#include <sys/random.h>

namespace kcf
{
// Lifecycle is controlled externally; do not Close/destroy inside feedback.
// Feedback callback runs on the existing Subscriber worker. Result uses Service.
template <typename Goal, typename Feedback, typename Result>
class ActionClient
{
    static_assert(std::is_trivially_copyable_v<Goal> && std::is_trivially_copyable_v<Feedback> && std::is_trivially_copyable_v<Result>);
    static_assert(!std::is_pointer_v<Goal> && !std::is_pointer_v<Feedback> && !std::is_pointer_v<Result>);
public:
    ~ActionClient() { Close(); }
    int Create(std::uint16_t port, ActionEndpointIds ids, const std::string& topic,
               std::function<void(const ActionStatus<Feedback>&)> callback,
               std::uint32_t timeout_ms = 200, std::uint32_t retries = 2)
    {
        if (open_) return -EBUSY;
        if (!ids.Valid() || !callback) return -EINVAL;
        if (!seeded_)
        {
            std::uint64_t seed = 0;
            ssize_t bytes;
            do { bytes = getrandom(&seed, sizeof(seed), 0); } while (bytes < 0 && errno == EINTR);
            if (bytes != sizeof(seed)) return bytes < 0 ? -errno : -EIO;
            next_id_ = seed & 0x7fffffffffffffffULL;
            seeded_ = true;
        }
        int error = service_.Create(port, timeout_ms, retries);
        if (error) return error;
        ids_ = ids;
        open_ = true;
        error = subscriber_.Create(topic, std::move(callback));
        if (error) { open_ = false; service_.Close(); return error; }
        return 0;
    }
    int SendGoal(const Goal& goal, std::uint64_t& goal_id)
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        if (!open_) return -EBADF;
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
        ActionGoalRequest<Goal> req{};
        req.goal_id = ++next_id_;
        req.goal = goal;
        ActionGoalResponse res{};
        int error = service_.Call(ids_.goal_service_id, req, res);
        if (error) return error;
        if (res.goal_id != req.goal_id || res.accepted > 1) return -EPROTO;
        if (res.result_code) return res.result_code;
        if (!res.accepted) return -ECANCELED;
        goal_id = req.goal_id;
        return 0;
    }
    int Cancel(std::uint64_t id)
    {
        if (!open_) return -EBADF;
        ActionCancelResponse res{};
        const int error = service_.Call(ids_.cancel_service_id, ActionCancelRequest{id}, res);
        if (error) return error;
        if (res.goal_id != id || res.accepted > 1) return -EPROTO;
        if (res.result_code) return res.result_code;
        return res.accepted ? 0 : -ECANCELED;
    }
    int GetResult(std::uint64_t id, Result& result, ActionState* state = nullptr)
    {
        if (!open_) return -EBADF;
        ActionResultResponse<Result> res{};
        const int error = service_.Call(ids_.result_service_id, ActionResultRequest{id}, res);
        if (error) return error;
        if (res.goal_id != id) return -EPROTO;
        if (res.result_code) return res.result_code;
        if (!res.ready) return -EINPROGRESS;
        if (res.ready != 1 || !IsActionTerminal(res.state)) return -EPROTO;
        result = res.result;
        if (state) *state = res.state;
        return 0;
    }
    int Close()
    {
        const int error = subscriber_.Close();
        if (error == -EDEADLK) return error;
        service_.Close();
        open_ = false;
        return error;
    }
private:
    ServiceClient service_;
    Subscriber<ActionStatus<Feedback>> subscriber_;
    ActionEndpointIds ids_{};
    std::mutex call_mutex_;
    std::uint64_t next_id_{0};
    bool seeded_{false}, open_{false};
};
}
