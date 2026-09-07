#pragma once
#include "kcf/action/action_types.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/service/service_server.hpp"
#include <memory>
#include <mutex>
#include <limits>

namespace kcf
{
// No application worker. Stop ServiceServer and application Loop/Timer before
// Unlink/Close. Unlink is explicit and precedes Close; destructor never unlinks.
// Service registrations hold weak state references, never a dangling `this`.
// Failed partial registration requires ServiceServer Create/register again.
template <typename Goal, typename Feedback, typename Result>
class ActionServer
{
    static_assert(std::is_trivially_copyable_v<Goal> && std::is_trivially_copyable_v<Feedback> && std::is_trivially_copyable_v<Result>);
    static_assert(!std::is_pointer_v<Goal> && !std::is_pointer_v<Feedback> && !std::is_pointer_v<Result>);
    struct Core
    {
        std::mutex mutex;
        Publisher<ActionStatus<Feedback>> publisher;
        ActionHeader header{};
        std::shared_ptr<const Goal> goal;
        std::shared_ptr<const Feedback> feedback = std::make_shared<Feedback>();
        std::shared_ptr<const Result> result;
        std::function<int(std::uint64_t, const Goal&)> on_goal;
        std::function<void(std::uint64_t)> on_cancel;
        std::uint64_t previous_id{0}, decision_id{0};
        int decision_code{0};
        std::uint64_t revision{0};
        bool publishing{false}; // under mutex; at most one caller drains status

        int Flush()
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (publishing) return 0;
            publishing = true;
            for (;;)
            {
                const auto observed = revision;
                const auto h = header;
                auto f = feedback;
                lock.unlock();
                ActionStatus<Feedback> snapshot{};
                snapshot.header = h;
                snapshot.feedback = *f; // large copy outside state lock
                int error;
                do { error = publisher.Publish(snapshot); if (error == -EAGAIN) std::this_thread::yield(); }
                while (error == -EAGAIN);
                lock.lock();
                if (error != 0 || observed == revision)
                {
                    publishing = false;
                    return error;
                }
                // A newer mutation arrived during Publish; emit the latest,
                // without permitting a stale concurrent writer to overwrite it.
            }
        }

        void GoalRequest(const ActionGoalRequest<Goal>& req, ActionGoalResponse& res)
        {
            res.goal_id = req.goal_id;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!req.goal_id) { res.result_code = -EINVAL; return; }
                if (req.goal_id == header.goal_id || req.goal_id == previous_id)
                { res.accepted = 1; return; }
                if (req.goal_id == decision_id)
                { res.result_code = decision_code; res.accepted = decision_code == 0; return; }
                if (header.state == ActionState::ACCEPTED || header.state == ActionState::RUNNING)
                { res.result_code = -EBUSY; return; }
            }
            auto g = std::make_shared<Goal>(req.goal);
            auto f = std::make_shared<Feedback>();
            int code;
            try { code = on_goal(req.goal_id, req.goal); }
            catch (...) { code = -EFAULT; }
            {
                std::lock_guard<std::mutex> lock(mutex);
                decision_id = req.goal_id;
                decision_code = code;
                res.result_code = code;
                if (code != 0) return;
                previous_id = header.goal_id;
                header = {};
                header.goal_id = req.goal_id;
                header.state = ActionState::ACCEPTED;
                goal = std::move(g);
                feedback = std::move(f);
                result.reset();
                ++revision;
                res.accepted = 1;
            }
            Flush();
        }

        void CancelRequest(const ActionCancelRequest& req, ActionCancelResponse& res)
        {
            res.goal_id = req.goal_id;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!req.goal_id || req.goal_id != header.goal_id) { res.result_code = -ENOENT; return; }
                if (header.cancel_requested) { res.accepted = 1; return; }
                if (header.state != ActionState::ACCEPTED && header.state != ActionState::RUNNING)
                { res.result_code = -EINVAL; return; }
                header.cancel_requested = 1;
                ++revision;
                res.accepted = 1;
            }
            try { if (on_cancel) on_cancel(req.goal_id); }
            catch (...) { res.result_code = -EFAULT; }
            Flush();
        }

        void GetResult(const ActionResultRequest& req, ActionResultResponse<Result>& res)
        {
            res.goal_id = req.goal_id;
            std::shared_ptr<const Result> saved;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!req.goal_id || req.goal_id != header.goal_id) { res.result_code = -ENOENT; return; }
                res.state = header.state;
                saved = result;
            }
            if (!saved) { res.result_code = -EINPROGRESS; return; }
            res.result = *saved;
            res.ready = 1;
        }
    };

public:
    ActionServer() = default;
    ActionServer(const ActionServer&) = delete;
    ActionServer& operator=(const ActionServer&) = delete;

    int Create(ServiceServer& server, ActionEndpointIds ids, const std::string& topic,
               std::function<int(std::uint64_t, const Goal&)> goal_callback,
               std::function<void(std::uint64_t)> cancel_callback = {})
    {
        if (core_) return -EBUSY;
        if (!ids.Valid() || !goal_callback) return -EINVAL;
        auto core = std::make_shared<Core>();
        core->on_goal = std::move(goal_callback);
        core->on_cancel = std::move(cancel_callback);
        int error = core->publisher.Create(topic);
        if (error != 0) return error;
        std::weak_ptr<Core> weak = core;
        error = server.Register<ActionGoalRequest<Goal>, ActionGoalResponse>(ids.goal_service_id,
            [weak](const auto& req, auto& res)
            { if (auto c = weak.lock()) c->GoalRequest(req, res); else { res.goal_id=req.goal_id; res.result_code=-ESHUTDOWN; } });
        if (!error) error = server.Register<ActionCancelRequest, ActionCancelResponse>(ids.cancel_service_id,
            [weak](const auto& req, auto& res)
            { if (auto c = weak.lock()) c->CancelRequest(req, res); else { res.goal_id=req.goal_id; res.result_code=-ESHUTDOWN; } });
        if (!error) error = server.Register<ActionResultRequest, ActionResultResponse<Result>>(ids.result_service_id,
            [weak](const auto& req, auto& res)
            { if (auto c = weak.lock()) c->GetResult(req, res); else { res.goal_id=req.goal_id; res.result_code=-ESHUTDOWN; } });
        if (!error) error = core->Flush();
        if (error) { core->publisher.Unlink(); return error; }
        core_ = std::move(core);
        return 0;
    }

    int Start(std::uint64_t id)
    {
        if (!core_) return -EBADF;
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if (!id || id != core_->header.goal_id) return -ENOENT;
            if (core_->header.state != ActionState::ACCEPTED || core_->header.cancel_requested) return -EINVAL;
            core_->header.state = ActionState::RUNNING;
            ++core_->revision;
        }
        return core_->Flush();
    }

    int PublishFeedback(std::uint64_t id, const Feedback& feedback)
    {
        if (!core_) return -EBADF;
        auto snapshot = std::make_shared<Feedback>(feedback);
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            if (!id || id != core_->header.goal_id) return -ENOENT;
            if (core_->header.state != ActionState::RUNNING) return -EINVAL;
            if (core_->header.feedback_sequence == std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
            core_->feedback = std::move(snapshot);
            ++core_->header.feedback_sequence;
            ++core_->revision;
        }
        return core_->Flush();
    }

    int Succeed(std::uint64_t id, const Result& result) { return Finish(id, result, ActionState::SUCCEEDED); }
    int Fail(std::uint64_t id, const Result& result) { return Finish(id, result, ActionState::FAILED); }
    int Canceled(std::uint64_t id, const Result& result) { return Finish(id, result, ActionState::CANCELED); }
    bool IsCancelRequested(std::uint64_t id) const
    {
        if (!core_) return false;
        std::lock_guard<std::mutex> lock(core_->mutex);
        return id != 0 && id == core_->header.goal_id && core_->header.cancel_requested;
    }
    ActionState GetState() const
    {
        if (!core_) return ActionState::IDLE;
        std::lock_guard<std::mutex> lock(core_->mutex);
        return core_->header.state;
    }
    std::uint64_t GetGoalId() const
    {
        if (!core_) return 0;
        std::lock_guard<std::mutex> lock(core_->mutex);
        return core_->header.goal_id;
    }
    int Unlink() { return core_ ? core_->publisher.Unlink() : -EBADF; }
    void Close() { core_.reset(); }

private:
    int Finish(std::uint64_t id, const Result& value, ActionState terminal)
    {
        if (!core_) return -EBADF;
        auto saved = std::make_shared<Result>(value);
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            const auto& h = core_->header;
            if (!id || id != h.goal_id) return -ENOENT;
            if (terminal == ActionState::CANCELED)
            {
                if (!h.cancel_requested || (h.state != ActionState::ACCEPTED && h.state != ActionState::RUNNING)) return -EINVAL;
            }
            else if (h.state != ActionState::RUNNING) return -EINVAL;
            core_->result = std::move(saved);
            core_->header.state = terminal;
            ++core_->revision;
        }
        return core_->Flush();
    }
    std::shared_ptr<Core> core_;
};
}
