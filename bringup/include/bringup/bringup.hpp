#pragma once

#include <signal.h>
#include <string>

#include "kcf/process/process.hpp"

namespace bringup
{

class Bringup
{
public:
    int Setup(const std::string& dummy_executable);
    int Run();
    void Shutdown();

private:
    void RestoreSignalHandlers();

    kcf::Process dummy_process_;
    bool running_{false};
    int exit_code_{0};

    struct sigaction previous_sigint_ {};
    struct sigaction previous_sigterm_ {};
    bool sigint_installed_{false};
    bool sigterm_installed_{false};
};

} // namespace bringup
