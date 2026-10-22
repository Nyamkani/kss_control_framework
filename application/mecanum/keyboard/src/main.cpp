#include "mecanum/messages.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_runtime.hpp"
#include "kcf/process/execution_mode.hpp"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <string>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {
std::uint64_t Now() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC,&t)) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
struct Options {
    double linear=0.15,angular=0.5;
    unsigned input_timeout_ms=150;
    std::string prefix="/mecanum";
};
class Keyboard final : public kcf::ProcessElement {
public:
    Keyboard(Options options,kcf::ProcessRuntime& runtime):options_(std::move(options)),runtime_(runtime) {}
    ~Keyboard() override { Cleanup(); }
    int Setup() override {
        if (!isatty(STDIN_FILENO)) return -ENOTTY;
        if (tcgetattr(STDIN_FILENO,&saved_)) return -errno;
        auto mode=saved_; mode.c_lflag&=~(ICANON|ECHO); // keep ISIG for Ctrl-C
        mode.c_cc[VMIN]=0; mode.c_cc[VTIME]=0;
        if (tcsetattr(STDIN_FILENO,TCSANOW,&mode)) return -errno;
        terminal_=true;
        const int rc=publisher_.Create(options_.prefix+"/cmd_vel",1);
        if (rc) return rc;
        owned_=true;
        std::cout<<"Keyboard Standalone: W/S forward/back, A/D left/right, Q/E yaw, Space stop, Esc exit\n"<<std::flush;
        return Send();
    }
    int Loop() override {
        pollfd p{STDIN_FILENO,POLLIN,0};
        if (poll(&p,1,0)<0) return errno==EINTR ? 0 : -errno;
        if (p.revents&(POLLERR|POLLNVAL|POLLHUP)) { Zero(); runtime_.RequestStop(); return Send(); }
        if (p.revents&POLLIN) {
            char keys[64]; const auto count=read(STDIN_FILENO,keys,sizeof(keys));
            if (count<0) return errno==EINTR || errno==EAGAIN ? 0 : -errno;
            if (!count) { Zero(); runtime_.RequestStop(); return Send(); }
            for (ssize_t i=0;i<count;++i) {
                const char key=keys[i];
                if (key==27 || key==3) { Zero(); runtime_.RequestStop(); return Send(); }
                if (key==' ') { Zero(); continue; }
                double vx=0,vy=0,wz=0;
                switch(key) {
                case 'w': case 'W': vx=options_.linear; break;
                case 's': case 'S': vx=-options_.linear; break;
                case 'a': case 'A': vy=options_.linear; break;
                case 'd': case 'D': vy=-options_.linear; break;
                case 'q': case 'Q': wz=options_.angular; break;
                case 'e': case 'E': wz=-options_.angular; break;
                default: continue; // unrelated input never extends motion
                }
                cmd_.vx_m_s=vx; cmd_.vy_m_s=vy; cmd_.wz_rad_s=wz; last_key_=Now();
            }
        }
        if (last_key_ && Now()-last_key_>=std::uint64_t(options_.input_timeout_ms)*1000000ULL) Zero();
        return Send();
    }
    void Shutdown() override {
        if (owned_) { Zero(); shutdown_error_=Send(); }
        const int rc=Cleanup(); if (!shutdown_error_) shutdown_error_=rc;
        if (shutdown_error_) std::cerr<<"Keyboard final zero/cleanup failed: "<<shutdown_error_<<'\n';
    }
    int ShutdownError() const { return shutdown_error_; }
private:
    void Zero() { cmd_.vx_m_s=cmd_.vy_m_s=cmd_.wz_rad_s=0; last_key_=0; }
    int Send() { ++cmd_.sequence; cmd_.generated_at_ns=Now(); return publisher_.Publish(cmd_); }
    int Cleanup() {
        int result=0;
        if (owned_) { result=publisher_.Close(); const int rc=publisher_.Unlink(); if (!result) result=rc; owned_=false; }
        if (terminal_) { if (tcsetattr(STDIN_FILENO,TCSANOW,&saved_) && !result) result=-errno; terminal_=false; }
        return result;
    }
    Options options_; kcf::ProcessRuntime& runtime_;
    kcf::Publisher<mecanum::CmdVel> publisher_; mecanum::CmdVel cmd_{};
    termios saved_{}; bool terminal_=false,owned_=false;
    std::uint64_t last_key_=0; int shutdown_error_=0;
};
bool Number(const char* text,double& number) {
    char* end=nullptr; errno=0; number=std::strtod(text,&end);
    return end!=text && *end=='\0' && !errno && std::isfinite(number) && number>0;
}
}
int main(int argc,char** argv) {
    try {
        Options options;
        for(int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if(key=="--help") {
                std::cout<<"kcf_mecanum_keyboard [--linear-speed 0.15] [--angular-speed 0.5] "
                           "[--input-timeout-ms 150] [--topic-prefix /mecanum]\n"
                           "Requires a terminal; standalone only. Speeds in m/s and rad/s.\n";
                return 0;
            }
            if(++i>=argc) return 2;
            double value=0;
            if(key=="--topic-prefix") options.prefix=argv[i];
            else if(!Number(argv[i],value)) return 2;
            else if(key=="--linear-speed") options.linear=value;
            else if(key=="--angular-speed") options.angular=value;
            else if(key=="--input-timeout-ms" && value>=20 && value<=250 && std::floor(value)==value) options.input_timeout_ms=static_cast<unsigned>(value);
            else return 2;
        }
        if(options.prefix.size()<2 || options.prefix.front()!='/' || options.prefix.back()=='/') return 2;
        if(kcf::DetectLaunchExecutionMode()!=kcf::ExecutionMode::STANDALONE) {
            std::cerr<<"Keyboard requires standalone execution outside Supervisor\n"; return 2;
        }
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(50);
        Keyboard keyboard(options,runtime);
        const int result=runtime.Run(keyboard);
        return result || keyboard.ShutdownError() ? 1 : 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
