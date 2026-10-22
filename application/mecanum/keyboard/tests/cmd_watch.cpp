#include "mecanum/messages.hpp"
#include "kcf/ipc/subscriber.hpp"
#include <chrono>
#include <thread>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    kcf::Subscriber<mecanum::CmdVel> sub;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while(sub.Create(std::string(argv[1])+"/cmd_vel")!=0) {
        if(std::chrono::steady_clock::now()>end) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::uint64_t previous=0;
    while(std::chrono::steady_clock::now()<end) {
        mecanum::CmdVel c{};kcf::TopicReadInfo info{};
        if(!sub.ReadLatest(c,info) && info.sequence!=previous) {
            previous=info.sequence;
            std::cout<<c.sequence<<' '<<c.generated_at_ns<<' '<<c.vx_m_s<<' '<<c.vy_m_s<<' '<<c.wz_rad_s<<'\n'<<std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
