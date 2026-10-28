#include "network_lifecycle.hpp"
#include <cassert>
#include <iostream>
using L=gateway::NetworkLifecycle;
using S=gateway::NetworkState;
using namespace std::chrono_literals;
struct Rig {
    int resolve_error=-ENETDOWN,start_error=0,worker_error=0,starts=0,stops=0,live=0;
    bool signal=false,signal_in_start=false,signal_in_stop=false;
    L::Selection selected{"nic","192.0.2.1",1,true};
    L machine;
    Rig():machine({[&](auto& out){out=selected;return resolve_error;},
        [&](const auto&){assert(!live);++starts;if(signal_in_start)signal=true;if(!start_error)++live;return start_error;},
        [&]{assert(live==1);--live;++stops;if(signal_in_stop)signal=true;},
        [&]{return worker_error;},[&]{return signal;},[](auto,const char*,int){}}){}
    void Tick(int seconds){machine.Step(L::Clock::time_point{}+std::chrono::seconds(seconds));}
};
int main(){
    {
        Rig r;r.Tick(0);r.Tick(1);assert(r.starts==0&&r.machine.State()==S::WAITING_NETWORK);
        r.resolve_error=0;r.Tick(2);assert(r.machine.State()==S::REBINDING);r.Tick(3);
        assert(r.machine.State()==S::RUNNING&&r.starts==1);
        for(int i=4;i<20;++i)r.Tick(i);
        assert(r.starts==1);
        r.resolve_error=-ENETDOWN;r.Tick(20);assert(!r.live&&r.stops==1);
        r.resolve_error=0;r.Tick(21);r.Tick(22);assert(r.live&&r.machine.Restarts()==1);
        r.selected.ipv4_address="192.0.2.2";r.Tick(23);assert(!r.live);r.Tick(24);assert(r.live&&r.starts==3);
        r.resolve_error=-ENOTUNIQ;r.Tick(25);assert(!r.live&&r.machine.State()==S::WAITING_NETWORK);
        r.Tick(26);assert(r.starts==3);r.resolve_error=0;r.Tick(27);r.Tick(28);assert(r.starts==4);
        for(int i=0;i<10;++i){r.resolve_error=-ENODEV;r.Tick(30+3*i);r.resolve_error=0;r.Tick(31+3*i);r.Tick(32+3*i);assert(r.live==1);}
        r.signal=true;r.Tick(60);assert(!r.live);r.Tick(61);assert(r.stops==r.starts);
    }
    {
        Rig r;r.resolve_error=0;r.start_error=-EIO;
        for(int i=0;i<100;++i)r.Tick(i);
        assert(r.machine.State()==S::ERROR&&r.starts==5&&r.machine.Attempts()==5&&!r.live);
    }
    {
        Rig r;r.resolve_error=-EINVAL;r.Tick(0);assert(r.machine.State()==S::ERROR&&!r.starts);
    }
    {
        Rig r;r.resolve_error=0;r.start_error=-EACCES;r.Tick(0);r.Tick(1);
        assert(r.machine.State()==S::ERROR&&r.starts==1);
    }
    {
        Rig r;r.resolve_error=0;r.Tick(0);r.signal=true;r.Tick(1);assert(!r.starts);
    }
    {
        Rig r;r.resolve_error=0;r.signal_in_start=true;r.Tick(0);r.Tick(1);assert(!r.live&&r.stops==1);
    }
    {
        Rig r;r.resolve_error=0;r.Tick(0);r.Tick(1);r.signal_in_stop=true;r.selected.index=2;r.Tick(2);
        assert(!r.live&&r.starts==1);
    }
    {
        Rig r;r.resolve_error=0;r.Tick(0);r.Tick(1);r.worker_error=-EIO;r.Tick(2);
        assert(!r.live);r.worker_error=0;r.Tick(3);r.Tick(4);assert(r.live&&r.starts==2);
        assert(r.machine.LastRecoveryError()==-EIO);
    }
    std::cout<<"Network lifecycle injected NIC absence/down/IP/ambiguity/stability/retry limits/signals/serialization PASS\n";
}
