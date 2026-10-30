#include "bringup/bringup.hpp"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <unistd.h>

namespace fs = std::filesystem;
using Specs = std::vector<bringup::ElementSpec>;
struct Resources {
    std::size_t fds;
    std::set<std::string> shm;
    std::string children;
    struct sigaction interrupt{}, terminate{};
    sigset_t mask{};
    Resources() {
        fds=std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator{});
        for(const auto& entry:fs::directory_iterator("/dev/shm")) shm.insert(entry.path().filename().string());
        std::ifstream in("/proc/self/task/"+std::to_string(getpid())+"/children");
        children.assign(std::istreambuf_iterator<char>(in),{});
        assert(sigaction(SIGINT,nullptr,&interrupt)==0);
        assert(sigaction(SIGTERM,nullptr,&terminate)==0);
        assert(pthread_sigmask(SIG_SETMASK,nullptr,&mask)==0);
    }
    void Same(const Resources& other) const {
        assert(fds==other.fds && shm==other.shm && children==other.children);
        assert(interrupt.sa_handler==other.interrupt.sa_handler && interrupt.sa_flags==other.interrupt.sa_flags);
        assert(terminate.sa_handler==other.terminate.sa_handler && terminate.sa_flags==other.terminate.sa_flags);
        for(int sig=1;sig<NSIG;++sig) {
            assert(sigismember(&mask,sig)==sigismember(&other.mask,sig));
            assert(sigismember(&interrupt.sa_mask,sig)==sigismember(&other.interrupt.sa_mask,sig));
            assert(sigismember(&terminate.sa_mask,sig)==sigismember(&other.terminate.sa_mask,sig));
        }
    }
};
static void Handler(int) {}
int main(int argc,char** argv) {
    assert(argc==2);alarm(20);
    struct sigaction previous_int{},previous_term{},custom{};
    custom.sa_handler=Handler;sigemptyset(&custom.sa_mask);sigaddset(&custom.sa_mask,SIGUSR1);
    custom.sa_flags=SA_RESTART;
    assert(sigaction(SIGINT,&custom,&previous_int)==0);
    assert(sigaction(SIGTERM,&custom,&previous_term)==0);
    const Resources baseline;
    const Specs valid{{"first",argv[1],{}},{"second",argv[1],{"--frequency","50"}}};
    bringup::Bringup app;
    bringup::RegistrationError error;
    auto untouched=[&] {
        baseline.Same(Resources{});
        assert(app.GetApplicationState()==bringup::ApplicationState::INITIALIZING);
        assert(app.GetElementStatuses().empty() && app.GetErrorHistory().empty());
        assert(!app.GetApplicationError().valid);
    };
    auto invalid=[&](const Specs& specs,std::optional<std::size_t> index,
                     const std::string& name,const std::string& field,const std::string& reason) {
        assert(bringup::Bringup::ValidateElements(specs,&error)==-EINVAL);
        assert(error.element_index==index && error.element_name==name);
        assert(error.field==field && error.reason==reason);
        assert(bringup::Bringup::ValidateElements(specs)==-EINVAL);
        assert(app.Setup("registration-validation",specs)==-EINVAL);
        untouched();
        assert(bringup::Bringup::ValidateElements(valid,&error)==0);
        assert(!error.element_index && error.element_name.empty() && error.field.empty() && error.reason.empty());
        untouched();
    };
    for(int repeat=0;repeat<3;++repeat) {
        assert(bringup::Bringup::ValidateElements(valid)==0);
        invalid({},std::nullopt,"","elements","empty");
        auto specs=valid;specs[1].name.clear();invalid(specs,1,"","name","empty");
        specs=valid;specs[1].executable.clear();invalid(specs,1,"second","executable","empty");
        specs=valid;specs[1].name="first";invalid(specs,1,"first","name","duplicate");
        specs=valid;specs[1].startup_timeout_ms=0;invalid(specs,1,"second","startup_timeout_ms","zero");
        specs=valid;specs[1].health_timeout_ms=0;invalid(specs,1,"second","health_timeout_ms","zero");
        specs=valid;specs[1].shutdown_timeout_ms=0;invalid(specs,1,"second","shutdown_timeout_ms","zero");
        specs=valid;specs[0].name.clear();specs[1].executable.clear();
        invalid(specs,0,"","name","empty"); // First element, first invalid field.
    }
    // Structural validation is not an executable or ExecutionProfile preflight.
    auto unchecked=valid;
    unchecked[0].executable="/nonexistent/kcf-validation-only";
    unchecked[0].execution_profile.priority=-1;
    unchecked[0].execution_profile.cpu_affinity={-1};
    assert(bringup::Bringup::ValidateElements(unchecked,&error)==0);
    untouched();

    assert(app.Setup("registration-validation",valid)==0);
    assert(app.GetApplicationState()==bringup::ApplicationState::RUNNING);
    const auto before=app.GetElementStatuses();
    const Resources active;
    assert(bringup::Bringup::ValidateElements({},&error)==-EINVAL);
    assert(app.Setup(Specs{})==-EBUSY); // Lifecycle check still takes precedence.
    active.Same(Resources{});
    const auto after=app.GetElementStatuses();
    assert(after.size()==before.size());
    for(std::size_t i=0;i<before.size();++i)
        assert(after[i].pid==before[i].pid && after[i].running==before[i].running);
    app.RequestShutdown();assert(app.Run()==0);
    assert(app.GetApplicationState()==bringup::ApplicationState::STOPPED);
    baseline.Same(Resources{});
    assert(sigaction(SIGINT,&previous_int,nullptr)==0);
    assert(sigaction(SIGTERM,&previous_term,nullptr)==0);
    std::cout<<"Registration rules/first error/clear output/repeat/side effects/Setup parity/EBUSY PASS\n";
}
