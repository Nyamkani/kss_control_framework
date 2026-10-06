#include "recovery/test_support.hpp"
#include "kcf/ipc/shared_channel.hpp"
#include "kcf/parameter/shared_parameter.hpp"

using namespace recovery_test;
using Clock = std::chrono::steady_clock;

namespace
{
// Linker interception is confined to this test executable. The real Open and
// real flock execute unchanged; only the consumer's scheduling is controlled.
thread_local bool gated_consumer = false;
std::atomic<bool> shared_locked{false}, release_shared{false};

template<class Predicate> void Until(Predicate predicate)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate())
    {
        assert(Clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Value { std::uint64_t generation, inverse; };
struct Topic
{
    using Channel = kcf::SharedChannel<Value>;
    static constexpr const char* label = "Topic";
    static int Create(Channel& c, const std::string& name, Value v)
    {
        const int result = c.Create(name);
        return result ? result : c.Publish(v);
    }
    static bool Check(Channel& c, Value expected)
    {
        Value value{}; std::uint32_t sequence = 0;
        return c.ReadLatestSnapshot(value, sequence) == 0 && sequence == 1 &&
               std::memcmp(&value, &expected, sizeof(value)) == 0;
    }
};
struct Parameter
{
    using Channel = kcf::SharedParameter<Value>;
    static constexpr const char* label = "Parameter";
    static int Create(Channel& c, const std::string& name, Value v) { return c.Create(name, v); }
    static bool Check(Channel& c, Value expected)
    {
        Value value{}; std::uint64_t version = 99;
        return c.Get(value, &version) == 0 && version == 0 &&
               std::memcmp(&value, &expected, sizeof(value)) == 0;
    }
};

// Capture the current real layout instead of introducing a second storage ABI.
template<class Kind> kcf::detail::OwnerHeader Header(const std::string& name, off_t& size)
{
    typename Kind::Channel owner;
    assert(Kind::Create(owner, name, {7, ~std::uint64_t{7}}) == 0);
    int fd = shm_open(name.c_str(), O_RDWR, 0600); assert(fd >= 0);
    struct stat st{}; assert(fstat(fd, &st) == 0); size = st.st_size;
    kcf::detail::OwnerHeader header{};
    assert(pread(fd, &header, sizeof(header), 0) == sizeof(header)); close(fd);
    assert(owner.Close() == 0); assert(owner.Unlink() == 0);
    return header;
}

template<class Kind> bool Run()
{
    const auto baseline = Fds();
    const auto name = "/kcf_incomplete_" + std::string(Kind::label) + "_" + std::to_string(getpid());
    off_t size = 0; const auto prototype = Header<Kind>(name, size);
    int failures = 0;
    // Four interrupted stages: empty, short header, valid header only, full size.
    // Each stage has one deterministic lock interleaving and 100 ungated races.
    for (int stage = 0; stage < 4; ++stage)
    {
        int stage_failures = 0, create_failures = 0, open_errors = 0, gated_result = 0;
        for (int cycle = 0; cycle <= 100; ++cycle)
        {
            int ready[2]; assert(pipe(ready) == 0);
            pid_t child = fork(); assert(child >= 0);
            if (child == 0)
            {
                close(ready[0]);
                int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600); assert(fd >= 0);
                assert(flock(fd, LOCK_EX) == 0);
                auto header = prototype;
                header.initialized = 0; header.owner_pid = getpid();
                if (stage == 3) assert(ftruncate(fd, size) == 0);
                if (stage != 0)
                {
                    const auto count = stage == 1 ? sizeof(header) / 2 : sizeof(header);
                    assert(pwrite(fd, &header, count, 0) == static_cast<ssize_t>(count));
                }
                assert(write(ready[1], "r", 1) == 1);
                for (;;) pause();
            }
            close(ready[1]); char byte;
            assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
            typename Kind::Channel owner;
            const Value fresh{static_cast<std::uint64_t>(stage * 101 + cycle + 100),
                              ~static_cast<std::uint64_t>(stage * 101 + cycle + 100)};
            assert(Kind::Create(owner, name, fresh) == -EEXIST); // active initializer
            int old_fd = shm_open(name.c_str(), O_RDWR, 0600); assert(old_fd >= 0);
            struct stat old_stat{}; assert(fstat(old_fd, &old_stat) == 0);
            assert(kill(child, SIGKILL) == 0); Reap(child);

            std::atomic<int> arrivals{0};
            std::atomic<bool> first_done{false}, created{false};
            shared_locked = false; release_shared = false;
            int create_result = -999, open_result = -999;
            bool payload_ok = false;
            std::thread consumer([&] {
                gated_consumer = cycle == 0;
                ++arrivals; Until([&] { return arrivals == 2; });
                typename Kind::Channel peer;
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                do
                {
                    open_result = peer.Open(name);
                    gated_consumer = false; // only the first Open is scheduled
                    first_done = true;
                    if (open_result == 0)
                    {
                        Until([&] { return created.load(); });
                        payload_ok = create_result == 0 && Kind::Check(peer, fresh);
                        assert(peer.Close() == 0);
                        break;
                    }
                    if (open_result != -EAGAIN && open_result != -ENOENT) break;
                    if (created && create_result != 0) break;
                    std::this_thread::yield();
                } while (Clock::now() < deadline);
                gated_consumer = false;
            });
            std::thread creator([&] {
                ++arrivals; Until([&] { return arrivals == 2; });
                if (cycle == 0) Until([&] { return shared_locked || first_done; });
                create_result = Kind::Create(owner, name, fresh); // exactly ONE startup attempt
                created = true;
                release_shared = true;
            });
            consumer.join(); creator.join();
            if (cycle == 0)
            {
                gated_result = create_result;
                std::cout << Kind::label << " stage=" << stage << " gated: LOCK_SH=" << shared_locked
                          << " Create=" << create_result << " Open=" << open_result << std::endl;
            }
            const bool passed = create_result == 0 && open_result == 0 && payload_ok;
            if (!passed) { ++failures; ++stage_failures; }
            if (create_result != 0) ++create_failures;
            if (open_result != 0 && open_result != -EAGAIN && open_result != -ENOENT) ++open_errors;
            // Failure cleanup/recovery is deliberately outside the measured attempt.
            if (create_result != 0) assert(Kind::Create(owner, name, fresh) == 0);
            int new_fd = shm_open(name.c_str(), O_RDWR, 0600); assert(new_fd >= 0);
            struct stat new_stat{}; assert(fstat(new_fd, &new_stat) == 0);
            assert(new_stat.st_ino != old_stat.st_ino); close(new_fd); close(old_fd);
            assert(owner.Close() == 0); assert(owner.Unlink() == 0); NoObject(name);
            assert(Fds() == baseline);
            int status; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
        }
        std::cout << Kind::label << " stage=" << stage << " failures=" << stage_failures
                  << "/101, Create failures=" << create_failures << ", nonretryable Open=" << open_errors
                  << ", deterministic Create=" << gated_result << std::endl;
    }
    // Live owner identity remains protected even without an initialization lock.
    // Unknown complete formats/magic and ambiguous PIDs must never be reclaimed.
    for (int mode = 0; mode < 5; ++mode)
    {
        auto header = prototype;
        header.owner_pid = getpid();
        if (mode == 0) header.initialized = 0;
        if (mode == 1) header.magic ^= 1;
        if (mode == 2) ++header.format;
        if (mode == 3) header.owner_pid = 0;
        if (mode == 4) header.owner_pid = -1;
        int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600); assert(fd >= 0);
        assert(ftruncate(fd, size) == 0);
        assert(pwrite(fd, &header, sizeof(header), 0) == sizeof(header));
        struct stat before{}, after{}; assert(fstat(fd, &before) == 0);
        typename Kind::Channel owner, peer;
        assert(Kind::Create(owner, name, {1, ~std::uint64_t{1}}) == -EEXIST);
        assert(peer.Open(name) < 0);
        int same = shm_open(name.c_str(), O_RDWR, 0600); assert(same >= 0);
        assert(fstat(same, &after) == 0 && before.st_ino == after.st_ino);
        kcf::detail::OwnerHeader unchanged{};
        assert(pread(same, &unchanged, sizeof(unchanged), 0) == sizeof(unchanged));
        assert(std::memcmp(&header, &unchanged, sizeof(header)) == 0);
        close(same); close(fd);
        assert(shm_unlink(name.c_str()) == 0); // fixture owns this exact corrupt object
        NoObject(name); assert(Fds() == baseline);
    }
    std::cout << Kind::label << " concurrent recovery " << (failures ? "FAIL" : "PASS")
              << ": 400 barrier races + 4 scheduled interleavings; active owner, fresh generation/payload,"
                 " FD/SHM/reap checks; failures=" << failures << std::endl;
    return failures == 0;
}
}

extern "C" int __real_flock(int, int);
extern "C" int __wrap_flock(int fd, int operation)
{
    const int result = __real_flock(fd, operation);
    if (result == 0 && operation == LOCK_SH && gated_consumer && !shared_locked.exchange(true))
        Until([] { return release_shared.load(); });
    return result;
}

int main(int argc, char** argv)
{
    alarm(60);
    assert(argc == 2);
    if (std::string(argv[1]) == "topic") return Run<Topic>() ? 0 : 1;
    assert(std::string(argv[1]) == "parameter");
    return Run<Parameter>() ? 0 : 1;
}
