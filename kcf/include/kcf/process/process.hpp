#pragma once

#include <string>
#include <sys/types.h>

namespace kcf
{

class Process
{
public:
    Process() = default;
    ~Process() = default;

    // A child PID must have a single owner.
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    int Start(const std::string& executable);
    int RequestStop();
    bool IsRunning();
    int Wait();
    pid_t GetPid() const;

private:
    // Returns 1 when reaped, 0 when running, or a negative errno.
    int CollectExit(int options);

    pid_t pid_{-1};
    bool exited_{false};
    int exit_status_{0};
};

} // namespace kcf
