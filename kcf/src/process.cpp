#include "kcf/process/process.hpp"

#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kcf
{

int Process::Start(const std::string& executable)
{
    if (pid_ > 0)
    {
        const int result = CollectExit(WNOHANG);
        if (result <= 0)
        {
            return result == 0 ? -EBUSY : result;
        }
    }

    const char* path = executable.c_str();
    const pid_t child = fork();
    if (child < 0)
    {
        return -errno;
    }
    if (child == 0)
    {
        execl(path, path, static_cast<char*>(nullptr));
        _exit(127);
    }

    pid_ = child;
    exited_ = false;
    exit_status_ = 0;
    return 0;
}

int Process::RequestStop()
{
    if (pid_ <= 0)
    {
        return -ESRCH;
    }

    const int result = CollectExit(WNOHANG);
    if (result != 0)
    {
        return result < 0 ? result : -ESRCH;
    }

    if (kill(pid_, SIGTERM) != 0)
    {
        return -errno;
    }
    return 0;
}

bool Process::IsRunning()
{
    return pid_ > 0 && CollectExit(WNOHANG) == 0;
}

int Process::Wait()
{
    if (exited_)
    {
        // IsRunning may already have collected exit_status_.
        return 0;
    }
    if (pid_ <= 0)
    {
        return -ECHILD;
    }

    const int result = CollectExit(0);
    return result < 0 ? result : 0;
}

pid_t Process::GetPid() const
{
    return pid_;
}

int Process::CollectExit(int options)
{
    int status = 0;
    pid_t result;
    do
    {
        result = waitpid(pid_, &status, options);
    } while (result < 0 && errno == EINTR);

    if (result < 0)
    {
        const int error = errno;
        if (error == ECHILD)
        {
            // The child is no longer waitable; do not signal a stale PID.
            pid_ = -1;
        }
        return -error;
    }
    if (result == 0)
    {
        return 0;
    }

    exit_status_ = status;
    exited_ = true;
    pid_ = -1;
    return 1;
}

} // namespace kcf
