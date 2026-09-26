// Test-only producer. One persistent Topic across scenarios, no hardware access.
#include "mecanum/messages.hpp"
#include "kcf/ipc/publisher.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <time.h>
std::uint64_t Now() { timespec t{}; clock_gettime(CLOCK_MONOTONIC,&t); return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec; }
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    kcf::Publisher<mecanum::CmdVel> pub;
    if(pub.Create(std::string(argv[1])+"/cmd_vel",1)) return 1;
    std::uint64_t sequence=0;
    double vx,vy,wz; unsigned duration;
    std::cout<<"READY\n"<<std::flush;
    int result=0;
    while(std::cin>>vx>>vy>>wz>>duration) {
        if(duration>10000) { result=2; break; }
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(duration);
        std::uint64_t first=0,last=0,count=0;
        do {
            mecanum::CmdVel c{++sequence,Now(),vx,vy,wz};
            if(pub.Publish(c)) { result=1; break; }
            if(!first) first=c.generated_at_ns;
            last=c.generated_at_ns; ++count;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while(std::chrono::steady_clock::now()<until);
        std::cout<<"DONE "<<first<<' '<<last<<' '<<count<<' '<<result<<'\n'<<std::flush;
        if(result) break;
        // No zero publication: the Motor's existing timeout must send zero.
    }
    pub.Close(); pub.Unlink(); return result;
}
