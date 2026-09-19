#pragma once
#include "kcf/system/system_status.hpp"
#include "kcf/system/system_status_scope.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include <atomic>
#include <thread>
#include <system_error>
#include <functional>
#include <utility>

namespace kcf
{
// Dedicated low-rate persistent state. Only the Supervisor holds this writer.
// No Parameter API is exposed to SystemStatus consumers.
class SystemStatusPublisher
{
public:
    int Create(const SystemStatus& initial) { return CreateScoped(initial,detail::InheritedSystemStatusScope()); }
    // Supervisor instance, independent of any scope inherited from its own parent.
    int CreateForApplication(const SystemStatus& initial) {
        std::string scope;const int result=detail::CurrentSystemStatusScope(scope);
        return result?result:CreateScoped(initial,scope);
    }
    const std::string& Scope() const { return scope_; }
    int Publish(const SystemStatus& value) { return storage_.Set(value); }
    int Close() { return storage_.Close(); }
    int Unlink() { return storage_.Unlink(); }
private:
    int CreateScoped(const SystemStatus& initial,const std::string& scope) {
        std::string name;int result=detail::SystemStatusName(scope,name);
        if(!result)result=storage_.Create(name,initial);
        if(!result)scope_=scope;
        return result;
    }
    std::string scope_;
    SharedParameter<SystemStatus> storage_;
};

// Initial/current and later snapshots are delivered on one worker, in order,
// outside shared locks. Slow readers may skip intermediate updates.
// Serialize Open/Close, finish reads before Close, and let callbacks return.
// A callback must not Close or destroy this subscriber.
class SystemStatusSubscriber
{
public:
    ~SystemStatusSubscriber() { Close(); }
    int Open(std::function<void(const SystemStatus&)> callback = {})
    { return OpenScoped(detail::InheritedSystemStatusScope(),std::move(callback)); }
    // Explicit public identity for observers outside the application's exec tree.
    int OpenForApplication(std::int32_t pid,std::uint64_t start_ticks,
                           std::function<void(const SystemStatus&)> callback = {}) {
        if(pid<=0 || !start_ticks)return -EINVAL;
        return OpenScoped(detail::SystemStatusScope(pid,start_ticks),std::move(callback));
    }
private:
    int OpenScoped(const std::string& scope,std::function<void(const SystemStatus&)> callback)
    {
        if (worker_.joinable()) return -EBUSY;
        std::string name;const int resolved=detail::SystemStatusName(scope,name);
        if(resolved)return resolved;
        const int opened=storage_.Open(name);
        if (opened) return opened;
        error_=0;
        if (!callback) return 0;
        SystemStatus initial{};std::uint64_t version=0;
        const int read=storage_.Get(initial,&version);
        if (read) { storage_.Close();return read; }
        running_=true;
        try
        {
            worker_=std::thread([this,initial,version,callback=std::move(callback)]() mutable
            {
                try
                {
                    if (running_) callback(initial);
                    while (running_)
                    {
                        SystemStatus value{};
                        const int result=storage_.Wait(value,version);
                        if (result==-ECANCELED) break;
                        if (result) { error_=result;break; }
                        if (running_) callback(value);
                    }
                }
                catch (...) { error_=-ECANCELED; }
                running_=false;
            });
        }
        catch (const std::system_error& error)
        { running_=false;storage_.Close();return -error.code().value(); }
        catch (...)
        { running_=false;storage_.Close();return -ENOMEM; }
        return 0;
    }
public:
    int ReadCurrent(SystemStatus& value) { return storage_.Get(value); }
    int Close()
    {
        if (worker_.joinable())
        {
            if (worker_.get_id()==std::this_thread::get_id()) return -EDEADLK;
            running_=false;
            const int stopped=storage_.StopWait();
            if (stopped && stopped!=-ENOTRECOVERABLE) return stopped;
            worker_.join();
            if (stopped) error_=stopped;
        }
        const int closed=storage_.Close();
        return closed ? closed : error_.load();
    }
private:
    SharedParameter<SystemStatus> storage_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> error_{0};
};
} // namespace kcf
