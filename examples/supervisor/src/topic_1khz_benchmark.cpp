#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/process/process.hpp"
#include "kcf/process/process_runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr std::uint64_t Second = 1000000000ULL;
volatile std::sig_atomic_t interrupted = 0;
void Signal(int) { interrupted = 1; }
std::uint64_t Now() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return std::uint64_t(ts.tv_sec) * Second + ts.tv_nsec;
}
std::size_t Fds() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
}
struct Options {
    unsigned duration{60}, warmup{5};
    std::string role, topic;
    std::uint64_t epoch{0};
    std::uint64_t Begin() const { return epoch + warmup * Second; }
    std::uint64_t End() const { return epoch + duration * Second; }
};
std::uint64_t Number(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected unsigned integer");
    return std::stoull(value);
}
void Usage() {
    std::cout << "Usage: kcf_topic_1khz_benchmark [--duration SEC] [--warmup SEC]\n"
        "Defaults: 60s total, first 5s excluded; 55s measurement, plus 3s startup.\n"
        "Duration 1..600, warmup < duration. Both children use OTHER/0, Runtime 1000Hz.\n"
        "Measures Publish timestamp to Callback entry, not Subscriber Loop latency.\n";
}
Options Parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (++i == argc) throw std::invalid_argument("missing option value");
        const std::string value = argv[i];
        if (key == "--duration" || key == "--warmup") {
            const auto n = Number(value);
            if (n > 600) throw std::invalid_argument("duration/warmup exceeds 600s");
            if (key == "--duration") o.duration = n; else o.warmup = n;
        } else if (key == "--child") o.role = value;
        else if (key == "--topic") o.topic = value;
        else if (key == "--epoch") o.epoch = Number(value);
        else throw std::invalid_argument("unknown option: " + key);
    }
    if (!o.duration || o.warmup >= o.duration) throw std::invalid_argument("invalid measurement window");
    if (!o.role.empty()) {
        if ((o.role != "publisher" && o.role != "subscriber") ||
            o.topic.rfind("/kcf_topic_bench_", 0) != 0 ||
            o.topic.find('/', 1) != std::string::npos || !o.epoch ||
            o.epoch > UINT64_MAX - 601 * Second)
            throw std::invalid_argument("invalid internal child arguments");
    } else if (!o.topic.empty() || o.epoch) throw std::invalid_argument("child options require --child");
    return o;
}
struct Message { std::uint64_t sequence; std::uint64_t timestamp_ns; };
struct Publishes {
    std::uint64_t success{0}, failed{0}, eagain{0};
    void Add(int code) { if (!code) ++success; else { ++failed; if (code == -EAGAIN) ++eagain; } }
};
struct Receives {
    std::uint64_t count{0}, first{0}, last{0}, gaps{0}, duplicates{0}, reversed{0};
    void Add(std::uint64_t sequence) {
        if (!count) first = last = sequence;
        else if (sequence == last) ++duplicates;
        else if (sequence < last) ++reversed;
        else { gaps += sequence - last - 1; last = sequence; }
        ++count;
    }
    void Print(std::ostream& out, const char* label) const {
        out << label << " count=" << count << " first=" << first << " last=" << last
            << " gaps=" << gaps << " duplicates=" << duplicates << " reversed=" << reversed << '\n';
    }
};
class Benchmark : public kcf::ProcessElement {
public:
    explicit Benchmark(const Options& o) : o_(o), delays_(o.role == "subscriber" ?
        std::size_t(o.duration) * 2000 + 1024 : 0) {}
    int Setup() override {
        if (o_.role == "publisher") {
            const int code = publisher_.Create(o_.topic); owned_ = code == 0; return code;
        }
        int code;
        do {
            code = subscriber_.Create(o_.topic, [this](const Message& message) { Receive(message); });
            if (code != -ENOENT && code != -EAGAIN) return code;
            std::this_thread::sleep_for(1ms);
        } while (Now() < o_.epoch);
        return -ETIMEDOUT;
    }
    int Loop() override {
        if (callback_error_.load()) return callback_error_.load();
        if (o_.role != "publisher") return 0;
        const auto now = Now();
        if (now < o_.epoch || now >= o_.End()) return 0;
        // Timestamp immediately before Publish; includes Publish and callback scheduling cost.
        const Message message{total_publish_.success + 1, now};
        const int code = publisher_.Publish(message);
        total_publish_.Add(code);
        if (now >= o_.Begin()) measured_publish_.Add(code);
        return code == -EAGAIN ? 0 : code;
    }
    void Shutdown() override {
        stopped_ = Now();
        if (o_.role == "publisher") {
            cleanup_error_ = publisher_.Close();
            if (owned_) {
                const int code = publisher_.Unlink();
                if (!cleanup_error_) cleanup_error_ = code;
                owned_ = false;
            }
        } else cleanup_error_ = subscriber_.Close(); // joins callback before Report reads its state
    }
    bool Report(int result) {
        const auto end = std::min(stopped_ ? stopped_ : Now(), o_.End());
        const double seconds = end > o_.Begin() ? double(end - o_.Begin()) / Second : 0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << "Child role=" << o_.role
            << " duration_s=" << o_.duration << " warmup_s=" << o_.warmup
            << " measurement_s=" << seconds << " runtime_result=" << result
            << " cleanup_error=" << cleanup_error_ << '\n';
        if (o_.role == "publisher") {
            auto print = [&](const char* label, const Publishes& p) {
                out << label << " success=" << p.success << " failed=" << p.failed
                    << " eagain=" << p.eagain << '\n';
            };
            print("Publish_total", total_publish_); print("Publish_measured", measured_publish_);
        } else {
            total_receive_.Print(out, "Receive_total"); measured_receive_.Print(out, "Receive_measured");
            out << "Callback samples=" << samples_ << " invalid=" << invalid_
                << " buffer_overflow=" << overflow_ << " receive_hz="
                << (seconds > 0 ? samples_ / seconds : 0) << '\n';
            if (samples_) {
                long double sum = 0;
                for (std::size_t i = 0; i < samples_; ++i) sum += delays_[i];
                std::sort(delays_.begin(), delays_.begin() + samples_);
                auto p = [&](double fraction) { return delays_[std::size_t(std::ceil(fraction * samples_)) - 1] / 1000.0; };
                out << "Publish_to_callback_us min=" << delays_[0] / 1000.0
                    << " mean=" << double(sum / samples_ / 1000) << " p50=" << p(.5)
                    << " p95=" << p(.95) << " p99=" << p(.99)
                    << " max=" << delays_[samples_ - 1] / 1000.0 << '\n';
            }
            out << "Samples require both publish timestamp and Callback entry in measurement window.\n"
                "Gaps between observed sequences are latest-value skips, not proof of IPC errors;\n"
                "initial/final unseen messages are not counted as gaps. Percentiles=nearest-rank.\n";
        }
        std::cout << out.str();
        const bool has_samples = stopped_ < o_.End() ||
            (o_.role == "publisher" ? measured_publish_.success != 0 : samples_ != 0);
        return has_samples && !result && !cleanup_error_ && !callback_error_.load() && !invalid_ && !overflow_ &&
            !total_receive_.duplicates && !total_receive_.reversed;
    }
private:
    void Receive(const Message& message) {
        const auto now = Now(); // first callback operation; no Loop observation latency
        total_receive_.Add(message.sequence);
        if (!message.sequence || message.timestamp_ns > now || message.timestamp_ns < o_.epoch) {
            ++invalid_; callback_error_.store(-EBADMSG); return;
        }
        if (message.timestamp_ns < o_.Begin() || now >= o_.End()) return;
        measured_receive_.Add(message.sequence);
        if (samples_ == delays_.size()) { overflow_ = true; callback_error_.store(-ENOSPC); return; }
        delays_[samples_++] = now - message.timestamp_ns;
    }
    Options o_;
    kcf::Publisher<Message> publisher_;
    kcf::Subscriber<Message> subscriber_;
    std::vector<std::uint64_t> delays_;
    Publishes total_publish_, measured_publish_;
    Receives total_receive_, measured_receive_;
    std::size_t samples_{0};
    std::uint64_t invalid_{0}, stopped_{0};
    bool owned_{false}, overflow_{false};
    int cleanup_error_{0};
    std::atomic<int> callback_error_{0};
};
struct Child {
    kcf::Process process;
    bool started{false}, pending{false}, running{false}, reaped{false};
    std::uint64_t request{0}, next{0}, sent{0}, heartbeat{0};
};
int Parent(Options o) {
    struct sigaction action{}; action.sa_handler = Signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) || sigaction(SIGTERM, &action, nullptr))
        throw std::runtime_error("sigaction failed");
    const auto baseline = Fds();
    o.epoch = Now() + 3 * Second;
    o.topic = "/kcf_topic_bench_" + std::to_string(getpid()) + "_" + std::to_string(o.epoch);
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    Child children[2];
    kcf::ExecutionProfile profile; profile.policy = kcf::SchedulingPolicy::OTHER;
    int error = 0;
    bool stopping = false, completed = false, forced = false;
    std::uint64_t stop_deadline = 0;
    try {
        for (int i = 0; i < 2; ++i) {
            auto& c = children[i];
            error = c.process.Start(executable, {"--child", i == 0 ? "publisher" : "subscriber",
                "--duration", std::to_string(o.duration), "--warmup", std::to_string(o.warmup),
                "--topic", o.topic, "--epoch", std::to_string(o.epoch)}, {}, profile);
            if (error) break;
            c.started = true;
            std::cout << "Started role=" << (i == 0 ? "publisher" : "subscriber")
                      << " pid=" << c.process.GetPid() << " topic=" << o.topic << '\n';
        }
        for (;;) {
            const auto now = Now();
            bool alive = false;
            for (auto& c : children) {
                if (!c.started || c.reaped) continue;
                if (!c.process.IsRunning()) {
                    c.reaped = true;
                    if (!stopping && !error) error = -ECHILD;
                    continue;
                }
                alive = true;
                if (stopping) continue;
                if (!c.pending && now >= c.next) {
                    const int code = c.process.RequestRuntimeStatus(++c.request);
                    if (!code) { c.pending = true; c.sent = now; }
                    else if (code != -EAGAIN && code != -EINTR) error = code;
                    c.next = now + Second / 10;
                }
                if (c.pending) {
                    kcf::RuntimeStatusResponse response;
                    const int code = c.process.ReceiveRuntimeStatus(response);
                    if (!code) {
                        c.pending = false;
                        c.running = response.state == kcf::ProcessState::RUNNING;
                        c.heartbeat = response.loop_heartbeat;
                        if (response.runtime_error) error = response.runtime_error;
                        if (response.state == kcf::ProcessState::ERROR && !error) error = -EFAULT;
                    } else if (code != -EAGAIN && code != -EINTR) error = code;
                    else if (now - c.sent > Second) error = -ETIMEDOUT;
                }
            }
            if (!alive) break;
            if (!stopping && now >= o.epoch && (!children[0].running || !children[1].running) && !error)
                error = -ETIMEDOUT;
            if (!stopping && (error || interrupted || now >= o.End())) {
                completed = !error && !interrupted && now >= o.End();
                stopping = true; stop_deadline = now + 5 * Second;
                for (auto& c : children) if (c.started && !c.reaped) {
                    const int code = c.process.RequestStop();
                    if (code && code != -ESRCH && !error) error = code;
                }
            }
            if (stopping && now >= stop_deadline) {
                for (auto& c : children) if (c.started && !c.reaped && c.process.IsRunning()) {
                    c.process.ForceStop(); forced = true;
                }
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        for (auto& c : children) if (c.started) c.process.Wait();
    } catch (...) {
        for (auto& c : children) if (c.started) { c.process.ForceStop(); c.process.Wait(); }
        throw;
    }
    bool clean = !error && !forced;
    for (int i = 0; i < 2; ++i) {
        auto& c = children[i]; const auto exit = c.process.GetExitInfo();
        const bool reaped = c.started && c.process.GetPid() == -1 && exit.available;
        clean = clean && reaped && exit.exited_normally && exit.exit_code == 0;
        std::cout << "Reaped role=" << (i == 0 ? "publisher" : "subscriber") << " reaped=" << reaped
            << " exit_code=" << exit.exit_code << " signal=" << exit.signal_number
            << " heartbeat=" << c.heartbeat << '\n';
    }
    const auto after = Fds();
    const bool absent = !std::filesystem::exists("/dev/shm" + o.topic);
    std::cout << "Parent completed=" << completed << " interrupted=" << bool(interrupted)
        << " forced=" << forced << " error=" << error << " fd_before=" << baseline
        << " fd_after=" << after << " shm_absent=" << absent << '\n';
    clean = clean && baseline == after && absent;
    // Never unlink an unverified mapping after abnormal child death.
    return interrupted ? (clean ? 130 : 1) : (clean && completed ? 0 : 1);
}
} // namespace
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc == 2 && std::string(argv[1]) == "--help") { Usage(); return 0; }
    Options o;
    try { o = Parse(argc, argv); }
    catch (const std::exception& e) { std::cerr << "Arguments: " << e.what() << '\n'; Usage(); return 2; }
    try {
        if (o.role.empty()) return Parent(o);
        const auto before = Fds();
        Benchmark element(o); // allocate all sample storage before Runtime Setup
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(1000.0);
        const int code = runtime.Run(element);
        const bool clean = element.Report(code);
        const auto after = Fds();
        const std::string fd_report = "Child_fds role=" + o.role + " before=" +
            std::to_string(before) + " after=" + std::to_string(after) + "\n";
        std::cout << fd_report;
        // Runtime consumes/closes the inherited supervision FD; no other descriptor may remain.
        return clean && (after == before || after + 1 == before) ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << "Benchmark error: " << e.what() << '\n'; return 1; }
}
