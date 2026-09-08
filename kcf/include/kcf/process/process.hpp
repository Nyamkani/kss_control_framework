#pragma once

#include "kcf/process/runtime_supervision.hpp"
#include <string>
#include <vector>
#include <sys/types.h>

namespace kcf
{

struct ProcessExitInfo
{
    bool available{false};
    bool exited_normally{false};
    int exit_code{0};
    bool signaled{false};
    int signal_number{0};
};

class Process
{
public:
    Process() = default;
    ~Process(); // closes its supervision fd, never kills/reaps implicitly

    // A child PID must have a single owner.
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    int Start(const std::string& executable, const std::vector<std::string>& args = {});
    int RequestStop();
    int ForceStop(); // explicit controlled cleanup only
    bool IsRunning();
    int Wait();
    pid_t GetPid() const;
    ProcessExitInfo GetExitInfo() const;
    int RequestRuntimeStatus(std::uint64_t request_id);
    int ReceiveRuntimeStatus(RuntimeStatusResponse& response);
    int GetSupervisionFd() const { return supervision_fd_; }

private:
    // Returns 1 when reaped, 0 when running, or a negative errno.
    int CollectExit(int options);

    pid_t pid_{-1};
    bool exited_{false};
    int exit_status_{0};
    int supervision_fd_{-1};
    std::uint64_t pending_request_{0};
    void CloseSupervision();
};

} // namespace kcf
