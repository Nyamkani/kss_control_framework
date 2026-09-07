#include "bringup/bringup.hpp"

#include <cerrno>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{

volatile sig_atomic_t stop_requested = 0;

void HandleStopSignal(int)
{
    stop_requested = 1;
}

} // namespace

namespace bringup
{

int Bringup::Setup(const std::string& dummy_executable)
{
    stop_requested = 0;
    exit_code_ = 0;

    struct sigaction action {};
    action.sa_handler = HandleStopSignal;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, &previous_sigint_) != 0)
    {
        std::cerr << "[Bringup] Failed to install SIGINT handler" << std::endl;
        return 1;
    }
    sigint_installed_ = true;

    if (sigaction(SIGTERM, &action, &previous_sigterm_) != 0)
    {
        std::cerr << "[Bringup] Failed to install SIGTERM handler" << std::endl;
        RestoreSignalHandlers();
        return 1;
    }
    sigterm_installed_ = true;

    const int result = dummy_process_.Start(dummy_executable);
    if (result != 0)
    {
        std::cerr << "[Bringup] Failed to start dummy: " << result << std::endl;
        RestoreSignalHandlers();
        return result;
    }

    std::cout << "[Bringup] Dummy process started" << std::endl;
    return 0;
}

int Bringup::Run()
{
    running_ = true;
    while (running_)
    {
        if (stop_requested != 0)
        {
            break;
        }
        if (!dummy_process_.IsRunning())
        {
            if (stop_requested == 0)
            {
                std::cerr << "[Bringup] Dummy process exited before shutdown request"
                          << std::endl;
                exit_code_ = 1;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Shutdown();
    return exit_code_;
}

void Bringup::Shutdown()
{
    if (dummy_process_.IsRunning())
    {
        const int result = dummy_process_.RequestStop();
        if (result == 0)
        {
            std::cout << "[Bringup] Sending SIGTERM to dummy" << std::endl;
        }
        else if (result != -ESRCH)
        {
            std::cerr << "[Bringup] Failed to request dummy stop: " << result
                      << std::endl;
            exit_code_ = 1;
        }
    }

    const int result = dummy_process_.Wait();
    if (result == 0)
    {
        std::cout << "[Bringup] Dummy process exited" << std::endl;
        std::cout << "[Bringup] Shutdown complete" << std::endl;
    }
    else
    {
        std::cerr << "[Bringup] Failed to wait for dummy: " << result << std::endl;
        exit_code_ = 1;
    }

    running_ = false;
    RestoreSignalHandlers();
}

void Bringup::RestoreSignalHandlers()
{
    if (sigterm_installed_)
    {
        sigaction(SIGTERM, &previous_sigterm_, nullptr);
        sigterm_installed_ = false;
    }
    if (sigint_installed_)
    {
        sigaction(SIGINT, &previous_sigint_, nullptr);
        sigint_installed_ = false;
    }
}

} // namespace bringup
