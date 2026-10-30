#pragma once
#include "kcf/network/network_interface.hpp"
#include <chrono>
#include <functional>
#include <algorithm>
#include <cerrno>

namespace gateway {
enum class NetworkState { WAITING_NETWORK, REBINDING, RUNNING, ERROR };
inline const char* StateName(NetworkState s) {
    switch(s){case NetworkState::WAITING_NETWORK:return "WAITING_NETWORK";case NetworkState::REBINDING:return "REBINDING";case NetworkState::RUNNING:return "RUNNING";case NetworkState::ERROR:return "ERROR";}return "ERROR";
}
// Gateway-only, caller-serialized state machine. Time and NIC/transport hooks are
// injectable without replacing any Framework Network implementation. No thread.
class NetworkLifecycle {
public:
    using Clock=std::chrono::steady_clock;
    using Selection=kcf::network::NetworkInterfaceSelection;
    struct Hooks {
        std::function<int(Selection&)> resolve;
        std::function<int(const Selection&)> start;
        std::function<void()> stop;
        std::function<int()> error;
        std::function<bool()> stopping;
        std::function<void(NetworkState,const char*,int)> report;
    };
    explicit NetworkLifecycle(Hooks hooks):hooks_(std::move(hooks)){}
    ~NetworkLifecycle(){Stop();}
    NetworkState State() const{return state_;}
    unsigned Attempts() const{return attempts_;}
    unsigned Restarts() const{return starts_?starts_-1:0;}
    int LastError() const{return last_error_;}
    int LastRecoveryError() const{return recovery_error_;}
    void Stop(){if(active_){hooks_.stop();active_=false;}pending_=false;}
    void Step(Clock::time_point now) {
        if(hooks_.stopping()){Stop();return;}
        if(state_==NetworkState::ERROR)return;
        if(active_){
            if(now-running_since_>=std::chrono::seconds(10))failures_=0;
            int code=hooks_.error();
            if(code){Stop();if(hooks_.stopping())return;Failure(now,code,"worker failure");return;}
        }
        if(now<next_poll_)return;
        next_poll_=now+std::chrono::seconds(1);
        Selection nic;int code=hooks_.resolve(nic);
        if(hooks_.stopping()){Stop();return;}
        if(code){
            Stop();if(hooks_.stopping())return;
            if(code==-ENETDOWN||code==-ENODEV||code==-EADDRNOTAVAIL||code==-ENOTUNIQ){
                Transition(NetworkState::WAITING_NETWORK,code== -ENOTUNIQ?"ambiguous interfaces":"network unavailable",code);return;
            }
            Failure(now,code,"resolver failure");return;
        }
        if(active_&&Same(nic,selected_))return;
        if(active_){Stop();if(hooks_.stopping())return;Transition(NetworkState::REBINDING,"interface/address changed",0);}
        if(!pending_||!Same(nic,candidate_)){
            candidate_=nic;pending_=true;stable_since_=now;
            Transition(NetworkState::REBINDING,"waiting for stable interface",0);return;
        }
        if(now-stable_since_<std::chrono::milliseconds(500)||now<retry_at_)return;
        if(hooks_.stopping())return;
        ++attempts_;Transition(NetworkState::REBINDING,"starting new network generation",0);
        code=hooks_.start(nic);
        // start must clean partial initialization itself on failure.
        if(hooks_.stopping()){if(!code)hooks_.stop();pending_=false;return;}
        if(code){Failure(now,code,"network start failed");return;}
        active_=true;selected_=nic;pending_=false;++starts_;running_since_=now;
        Transition(NetworkState::RUNNING,"new generation ready",0);
    }
private:
    static bool Same(const Selection& a,const Selection& b){return a.name==b.name&&a.index==b.index&&a.ipv4_address==b.ipv4_address;}
    static bool Transient(int code){return code==-EAGAIN||code==-EIO||code==-ETIMEDOUT||code==-ENETDOWN||code==-ENODEV||code==-EADDRNOTAVAIL||code==-ENETUNREACH||code==-EHOSTUNREACH;}
    void Failure(Clock::time_point now,int code,const char* reason){
        ++failures_;
        if(!Transient(code)||failures_>=5){Transition(NetworkState::ERROR,reason,code);return;}
        retry_at_=now+std::chrono::seconds(std::min(8u,1u<<(failures_-1)));
        next_poll_=retry_at_;Transition(NetworkState::WAITING_NETWORK,reason,code);
    }
    void Transition(NetworkState next,const char* reason,int error){
        const bool changed=next!=state_||error!=last_error_;
        state_=next;last_error_=error;if(error)recovery_error_=error;
        if(changed||next==NetworkState::REBINDING||next==NetworkState::RUNNING)hooks_.report(next,reason,error);
    }
    Hooks hooks_;NetworkState state_{NetworkState::WAITING_NETWORK};
    bool active_{false},pending_{false};unsigned attempts_{0},starts_{0},failures_{0};int last_error_{0},recovery_error_{0};
    Selection selected_,candidate_;Clock::time_point next_poll_{},retry_at_{},stable_since_{},running_since_{};
};
}
