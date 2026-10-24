#include "kcf/ipc/command_freshness.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include <cassert>
#include <iostream>
#include <unistd.h>
using namespace std::chrono_literals;
struct MockClock {
    using duration=std::chrono::nanoseconds;using rep=duration::rep;using period=duration::period;
    using time_point=std::chrono::time_point<MockClock>;static constexpr bool is_steady=true;
    inline static time_point current{};static time_point now() noexcept{return current;}
};
using Monitor=kcf::BasicCommandFreshnessMonitor<MockClock>;
using State=kcf::CommandFreshnessState;
struct Command {double velocity;};
int main(){
    Monitor monitor(100ms);auto at=[](int ms){MockClock::current=MockClock::time_point{}+std::chrono::milliseconds(ms);};
    assert(monitor.GetSnapshot().state==State::INVALID);
    assert(monitor.Observe(1));at(99);assert(!monitor.Observe(1));assert(monitor.GetSnapshot().state==State::VALID);
    at(100);assert(monitor.GetSnapshot().state==State::EXPIRED);at(101);assert(!monitor.Observe(1));assert(monitor.GetSnapshot().state==State::EXPIRED);
    assert(monitor.Observe(2));assert(monitor.GetSnapshot().age==0ms);
    monitor.ObserveRead(-EAGAIN,{});assert(monitor.GetSnapshot().state==State::VALID);
    monitor.ObserveRead(-ESTALE,{});assert(monitor.GetSnapshot().state==State::INVALID);assert(!monitor.Observe(3));
    monitor.Reset();assert(monitor.Observe(1));assert(!monitor.Observe(0));assert(monitor.GetSnapshot().state==State::INVALID);
    monitor.Reset();assert(monitor.Observe(10));assert(!monitor.Observe(9));assert(monitor.GetSnapshot().state==State::INVALID);
    Monitor bad(0ms);assert(!bad.Observe(1)&&bad.GetSnapshot().state==State::INVALID);
    monitor.Reset();at(0);
    const auto name="/command_watchdog_"+std::to_string(getpid());
    kcf::Publisher<Command> publisher;assert(!publisher.Create(name));assert(!publisher.Publish({5.0}));
    kcf::Subscriber<Command> subscriber;assert(!subscriber.Create(name,kcf::TopicStartPosition::NEXT));
    Command input{},last{};kcf::TopicReadInfo info;
    assert(subscriber.ReadNext(input,info)==-EAGAIN); // retained proxy-like sample ignored
    unsigned valid=0,expired=0;
    // Deterministic 1 ms control ticks / 10 ms command commits. No RT claim.
    for(int tick=0;tick<600;++tick){at(tick);
        if(tick%10==0 && (tick<200 || tick>=400))assert(!publisher.Publish({5.0}));
        const int result=subscriber.ReadNext(input,info);
        if(monitor.ObserveRead(result,info))last=input;
        const auto state=monitor.GetSnapshot().state;
        const double output=state==State::VALID?last.velocity:0.0; // Application owns fail-safe
        if(tick<290 || tick>=400){assert(output==5.0);++valid;}
        else {assert(state==State::EXPIRED&&output==0.0);++expired;}
    }
    assert(valid&&expired);
    subscriber.Close();monitor.Reset();assert(!subscriber.Create(name,kcf::TopicStartPosition::NEXT));
    assert(subscriber.ReadNext(input,info)==-EAGAIN);assert(monitor.GetSnapshot().state==State::INVALID);
    assert(!publisher.Publish({5.0}));assert(!subscriber.ReadNext(input,info));assert(monitor.ObserveRead(0,info));
    // Closed storage is an actual Subscriber error; caller must invalidate it.
    subscriber.Close();int error=subscriber.ReadNext(input,info);assert(error&&error!=-EAGAIN);
    monitor.ObserveRead(error,info);assert(monitor.GetSnapshot().state==State::INVALID);
    publisher.Close();publisher.Unlink();
    std::cout<<"Mock boundary / duplicate / errors / NEXT retained sample / 1kHz logical control + 100Hz commits / zero-output expiry / reconnect recovery PASS\n";
}
