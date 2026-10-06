#ifdef NDEBUG
#undef NDEBUG
#endif
#include "kcf/introspection/introspection_client.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/parameter/shared_parameter.hpp"
#include "kcf/process/process_runtime.hpp"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
int lock_notice = -1; // set only in the external observer child
void Send(int fd, int value) { assert(write(fd, &value, sizeof(value)) == sizeof(value)); }
int Receive(int fd)
{
    pollfd event{fd, POLLIN, 0};
    assert(poll(&event, 1, 5000) == 1);
    int value = 0;
    assert(read(fd, &value, sizeof(value)) == sizeof(value));
    return value;
}
bool Find(pid_t pid, kcf::RuntimeInfo* out = nullptr)
{
    kcf::IntrospectionClient client;
    std::vector<kcf::RuntimeInfo> values;
    assert(client.ListRuntimes(values) == 0);
    for (const auto& value : values)
        if (value.pid == pid) { if (out) *out = value; return true; }
    return false;
}
kcf::RuntimeInfo Await(pid_t pid, kcf::ProcessState state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    kcf::RuntimeInfo result;
    while (!Find(pid, &result) || result.state != state)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return result;
}
struct Element : kcf::ProcessElement
{
    int setup_error, loop_error, notice, release;
    bool notify_loop;
    Element(int setup, int loop, int n, int r, bool notify) : setup_error(setup), loop_error(loop), notice(n), release(r), notify_loop(notify) {}
    int Setup() override { return setup_error; }
    int Loop() override { if (notify_loop) { Send(notice, 6); notify_loop = false; } return loop_error; }
    void Shutdown() override
    {
        Send(notice, 7);
        assert(Receive(release) == 8); // expose STOPPING/ERROR until parent has inspected it
    }
};
struct Child
{
    pid_t pid;
    int command[2], notice[2];
    std::uint64_t ticks = 0;
    std::string name;
    Child(int setup = 0, int loop = 0, int supervision = -1, bool notify_loop = false)
    {
        assert(pipe(command) == 0 && pipe(notice) == 0);
        pid = fork(); assert(pid >= 0);
        if (pid == 0)
        {
            alarm(15);
            close(command[1]); close(notice[0]);
            // Exercise Linux comm containing spaces and ')' in the real parser.
            assert(prctl(PR_SET_NAME, "r1 ) space") == 0);
            if (supervision < 0) assert(unsetenv(kcf::SUPERVISION_FD_ENV) == 0);
            else assert(setenv(kcf::SUPERVISION_FD_ENV, std::to_string(supervision).c_str(), 1) == 0);
            assert(Receive(command[0]) == 1);
            kcf::ProcessRuntime runtime;
            runtime.SetLoopFrequency(500);
            Element element(setup, loop, notice[1], command[0], notify_loop);
            Send(notice[1], runtime.Run(element));
            _exit(0);
        }
        close(command[0]); close(notice[1]);
        assert(kcf::detail::ReadProcessIdentity(pid, ticks));
        name = kcf::detail::RuntimeRegistryName(pid, ticks);
    }
    void Start() { Send(command[1], 1); }
    void Reap(bool killed = false)
    {
        int status = 0; assert(waitpid(pid, &status, 0) == pid);
        assert(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL : WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    void Finish(int expected)
    {
        assert(Receive(notice[0]) == 7);
        Send(command[1], 8);
        assert(Receive(notice[0]) == expected);
        Reap();
    }
    ~Child()
    {
        close(command[1]); close(notice[0]);
        // Only test-owned names, including deliberately crashed runtime storage.
        shm_unlink(name.c_str());
        shm_unlink(kcf::detail::EndpointRegistryName(pid, ticks).c_str());
    }
};
void MissingStorage(const std::string& name)
{
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    assert(fd < 0 && errno == ENOENT);
}
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex)
{
    const int result = __real_pthread_mutex_lock(mutex);
    if (result == 0 && lock_notice >= 0)
    {
        Send(lock_notice, 9);
        for (;;) pause(); // parent kills this reader while the actual mutex is held
    }
    return result;
}
int main()
{
    alarm(60);
    std::uint64_t ticks = 0; char state = 0;
    std::string stat = "123 (name ) with spaces) S";
    for (int i = 4; i <= 21; ++i) stat += " 0";
    stat += " 987654 0";
    assert(kcf::detail::ParseProcessStat(stat, ticks, state) && ticks == 987654 && state == 'S');
    assert(!kcf::detail::ParseProcessStat("bad", ticks, state));
    std::int32_t parsed_pid = 0;
    assert(!kcf::detail::ParseRuntimeName("unrelated_1_2", parsed_pid, ticks));
    assert(!kcf::detail::ParseRuntimeName("kcf_introspection_runtime_1_2junk", parsed_pid, ticks));
    assert(!kcf::detail::ParseRuntimeName("kcf_introspection_runtime_2147483648_2", parsed_pid, ticks));
    {
        Child child;
        child.Start();
        const auto info = Await(child.pid, kcf::ProcessState::RUNNING);
        assert(info.process_start_ticks == child.ticks && info.process_start_ticks != 0);
        assert(info.execution_mode == kcf::ExecutionMode::STANDALONE && info.runtime_error == 0);
        assert(info.executable[0] && !std::strchr(info.executable, '/'));
        std::cout << "PASS A: standalone discovery\n";
        for (int crash = 0; crash < 2; ++crash)
        {
            int observed[2]; assert(pipe(observed) == 0);
            const pid_t observer = fork(); assert(observer >= 0);
            if (observer == 0)
            {
                close(observed[0]);
                assert(Find(child.pid));
                lock_notice = observed[1];
                (void)Find(child.pid);
                _exit(1);
            }
            close(observed[1]);
            assert(Receive(observed[0]) == 9);
            assert(kill(observer, SIGKILL) == 0);
            int status = 0; assert(waitpid(observer, &status, 0) == observer && WIFSIGNALED(status));
            close(observed[0]);
            if (crash == 0) assert(Await(child.pid, kcf::ProcessState::RUNNING).runtime_error == 0);
        }
        assert(kill(child.pid, SIGTERM) == 0);
        // No parent Get after the second crash: Runtime Set itself must recover.
        assert(Receive(child.notice[0]) == 7);
        assert(Await(child.pid, kcf::ProcessState::STOPPING).runtime_error == 0);
        Send(child.command[1], 8);
        assert(Receive(child.notice[0]) == 0);
        child.Reap();
        assert(!Find(child.pid)); MissingStorage(child.name);
        std::cout << "PASS B/D: observer lock-holder SIGKILL isolation and normal cleanup\n";
    }
    {
        Child child; child.Start(); Await(child.pid, kcf::ProcessState::RUNNING);
        assert(kill(child.pid, SIGKILL) == 0);
        // Zombie is already dead, even before waitpid reaps the PID.
        siginfo_t death{};
        assert(waitid(P_PID, child.pid, &death, WEXITED | WNOWAIT) == 0);
        assert(!Find(child.pid));
        child.Reap(true);
        assert(!Find(child.pid));
        const int fd = shm_open(child.name.c_str(), O_RDONLY, 0); assert(fd >= 0); close(fd);
        std::cout << "PASS C: zombie/dead filtering without client unlink\n";
    }
    for (const bool setup : {true, false})
    {
        Child child(setup ? -EIO : 0, setup ? 0 : -EIO);
        child.Start();
        assert(Await(child.pid, kcf::ProcessState::ERROR).runtime_error == -EIO);
        child.Finish(-EIO); MissingStorage(child.name);
        std::cout << "PASS: " << (setup ? "Setup" : "Loop") << " error precedence and cleanup\n";
    }
    {
        Child child(0, 0, -1, true);
        kcf::SharedParameter<kcf::RuntimeInfo> collision;
        kcf::RuntimeInfo wrong{}; // live parent owns collision; child cannot reclaim it
        assert(collision.Create(child.name, wrong) == 0);
        child.Start();
        assert(Receive(child.notice[0]) == 6); // actual Loop reached despite Begin failure
        assert(!Find(child.pid)); // mismatched snapshot identity must not be accepted
        assert(kill(child.pid, SIGTERM) == 0);
        child.Finish(0); // Setup/Loop ran and SIGTERM shutdown still succeeds
        assert(collision.Get(wrong) == 0 && wrong.pid == 0);
        assert(collision.Unlink() == 0); assert(collision.Close() == 0);
        std::cout << "PASS: registry creation failure leaves Runtime result unchanged\n";
    }
    {
        int sockets[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
        Child child(0, 0, sockets[1]);
        close(sockets[1]); child.Start();
        assert(Await(child.pid, kcf::ProcessState::RUNNING).execution_mode == kcf::ExecutionMode::SUPERVISED);
        kcf::RuntimeStatusRequest request{}; request.request_id = 1;
        assert(send(sockets[0], &request, sizeof(request), MSG_NOSIGNAL) == sizeof(request));
        pollfd event{sockets[0], POLLIN, 0}; assert(poll(&event, 1, 1000) == 1);
        kcf::RuntimeStatusResponse response{};
        assert(recv(sockets[0], &response, sizeof(response), 0) == sizeof(response));
        assert(response.pid == child.pid);
        assert(kill(child.pid, SIGTERM) == 0); child.Finish(0); close(sockets[0]);
        std::cout << "PASS: supervised execution mode before environment consumption\n";
    }
    {
        const auto pid = static_cast<std::int32_t>(getpid());
        assert(kcf::detail::ReadProcessIdentity(pid, ticks));
        const auto name = kcf::detail::RuntimeRegistryName(pid, ticks);
        kcf::SharedParameter<kcf::RuntimeInfo> owner;
        kcf::RuntimeInfo info{}; info.pid = pid; info.process_start_ticks = ticks;
        info.protocol_version = 2;
        assert(owner.Create(name, info) == 0);
        assert(!Find(pid));
        info.protocol_version = 1; info.struct_size = 0; assert(owner.Set(info) == 0); assert(!Find(pid));
        info.struct_size = sizeof(info); info.process_start_ticks++; assert(owner.Set(info) == 0); assert(!Find(pid));
        info.process_start_ticks = ticks; std::memset(info.executable, 'x', sizeof(info.executable));
        assert(owner.Set(info) == 0); assert(!Find(pid));
        assert(owner.Unlink() == 0); assert(owner.Close() == 0);
        // A well-formed payload under an old generation name must also be skipped.
        info.executable[0] = 0;
        const auto stale = kcf::detail::RuntimeRegistryName(pid, ticks + 1);
        kcf::SharedParameter<kcf::RuntimeInfo> old;
        assert(old.Create(stale, info) == 0); assert(!Find(pid));
        assert(old.Unlink() == 0); assert(old.Close() == 0);
        kcf::IntrospectionClient client;
        std::vector<kcf::RuntimeInfo> values{info};
        assert(client.ListRuntimes(values) == 0);
        for (const auto& value : values) assert(value.pid != pid);
        std::cout << "PASS: protocol/size/name/snapshot generation/string validation and output replacement\n";
    }
    std::cout << "R1 runtime discovery PASS\n";
}
