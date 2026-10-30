#include "kcf/process/process.hpp"
#include "kcf/process/process_runtime.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
volatile std::sig_atomic_t interrupted = 0;
void Signal(int) { interrupted = 1; }

struct Options {
    int duration{60}, warmup{5};
    bool child{false}, trace_outliers{false};
    kcf::ExecutionProfile profile;
    Options() { profile.policy = kcf::SchedulingPolicy::OTHER; }
};

int Number(const std::string& text, int maximum) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected nonnegative integer");
    const auto n = std::stoul(text);
    if (n > static_cast<unsigned>(maximum)) throw std::invalid_argument("argument out of range");
    return static_cast<int>(n);
}

void Usage() {
    std::cout << "Usage: kcf_runtime_1khz_benchmark [--duration SEC] [--warmup SEC]\n"
        "       [--policy other|inherit|fifo|rr] [--priority N] [--cpu ID[,ID...]]\n"
        "       [--trace-outliers] (default OFF; post-run top 20 with +/-5 samples)\n"
        "Defaults: duration=60s total, first 5s excluded (55s analysis window), OTHER/0.\n"
        "Duration: 1..3600; warmup: 0..duration-1. No automatic privilege elevation.\n"
        "Intervals >1500us are period exceedances, NOT exact deadline misses.\n";
}

Options Parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--trace-outliers") { o.trace_outliers = true; continue; }
        if (key == "--child") { o.child = true; continue; }
        if (i + 1 == argc) throw std::invalid_argument("missing argument value");
        const std::string value = argv[++i];
        if (key == "--duration") o.duration = Number(value, 3600);
        else if (key == "--warmup") o.warmup = Number(value, 3599);
        else if (key == "--priority") o.profile.priority = Number(value, 999);
        else if (key == "--policy") {
            if (value == "other") o.profile.policy = kcf::SchedulingPolicy::OTHER;
            else if (value == "inherit") o.profile.policy = kcf::SchedulingPolicy::INHERIT;
            else if (value == "fifo") o.profile.policy = kcf::SchedulingPolicy::FIFO;
            else if (value == "rr") o.profile.policy = kcf::SchedulingPolicy::RR;
            else throw std::invalid_argument("unknown scheduling policy");
        } else if (key == "--cpu") {
            std::size_t begin = 0;
            do {
                const auto end = value.find(',', begin);
                o.profile.cpu_affinity.push_back(Number(value.substr(begin, end - begin), CPU_SETSIZE - 1));
                if (end == std::string::npos) break;
                begin = end + 1;
            } while (true);
        } else throw std::invalid_argument("unknown option: " + key);
    }
    if (o.duration <= 0 || o.warmup >= o.duration) throw std::invalid_argument("invalid duration/warmup");
    return o;
}

int Monotonic(std::int64_t& ns) {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -errno;
    ns = std::int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    return 0;
}

int PrintScheduling() {
    const int policy = sched_getscheduler(0);
    sched_param parameters{};
    cpu_set_t mask; CPU_ZERO(&mask);
    if (policy < 0 || sched_getparam(0, &parameters) || sched_getaffinity(0, sizeof(mask), &mask))
        return -errno;
    const int base = policy & ~SCHED_RESET_ON_FORK;
    const char* name = base == SCHED_OTHER ? "OTHER" : base == SCHED_FIFO ? "FIFO" :
        base == SCHED_RR ? "RR" : "OTHER_LINUX_POLICY";
    std::cout << "Applied pid=" << getpid() << " scheduler=" << name << " policy_id=" << policy
              << " priority=" << parameters.sched_priority << " cpu_affinity=";
    bool first = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &mask)) {
        std::cout << (first ? "" : ",") << cpu; first = false;
    }
    std::cout << '\n';
    return 0;
}

class Benchmark : public kcf::ProcessElement {
public:
    explicit Benchmark(const Options& options) : options_(options),
        intervals_(static_cast<std::size_t>(options.duration) * 4000 + 1024),
        trace_(options.trace_outliers ? intervals_.size() : 0) {}
    int Setup() override { return PrintScheduling(); }
    int Loop() override {
        std::int64_t now{0};
        const int error = Monotonic(now);
        if (error) return error;
        const int cpu = options_.trace_outliers ? sched_getcpu() : -1;
        if (options_.trace_outliers && cpu < 0) return -errno;
        ++loops_;
        const bool first = !started_;
        if (first) { started_ = true; first_ = previous_ = now; }
        if (now < previous_) return -EIO;
        // Exclude intervals crossing either boundary; collect Loop start-to-start.
        const bool measured = !first &&
            previous_ - first_ >= std::int64_t(options_.warmup) * 1000000000LL &&
            now - first_ <= std::int64_t(options_.duration) * 1000000000LL;
        if (options_.trace_outliers) {
            if (trace_count_ == trace_.size()) { overflow_ = true; return -ENOSPC; }
            // Include warm-up/tail context, but never rank/count it as measured data.
            trace_[trace_count_++] = {now, now - previous_, cpu, measured};
        }
        if (measured) {
            if (samples_ == intervals_.size()) { overflow_ = true; return -ENOSPC; }
            intervals_[samples_++] = now - previous_;
        }
        previous_ = now;
        return 0;
    }
    void Report(int runtime_result) {
        std::cout << std::fixed << std::setprecision(3)
                  << "Result duration_s=" << options_.duration << " warmup_s=" << options_.warmup
                  << " loops=" << loops_ << " samples=" << samples_
                  << " buffer_overflow=" << overflow_ << " runtime_result=" << runtime_result << '\n';
        if (!samples_) {
            std::cout << "Statistics unavailable: no post-warmup intervals\n";
            std::cout << "Measured_exceedances gt_2ms=0 gt_5ms=0 gt_10ms=0 gt_100ms=0\n";
            if (options_.trace_outliers) ReportTrace();
            return;
        }
        long double sum = 0;
        std::uint64_t exceedances = 0, gt2 = 0, gt5 = 0, gt10 = 0, gt100 = 0;
        for (std::size_t i = 0; i < samples_; ++i) {
            sum += intervals_[i];
            if (intervals_[i] > 1500000) ++exceedances;
            if (intervals_[i] > 2000000) ++gt2;
            if (intervals_[i] > 5000000) ++gt5;
            if (intervals_[i] > 10000000) ++gt10;
            if (intervals_[i] > 100000000) ++gt100;
        }
        std::sort(intervals_.begin(), intervals_.begin() + samples_);
        auto percentile = [&](double p) {
            const auto index = static_cast<std::size_t>(std::ceil(p * samples_)) - 1;
            return intervals_[index] / 1000.0; // nearest-rank percentile
        };
        const double mean = static_cast<double>(sum / samples_ / 1000);
        auto row = [&](const char* label, double offset) {
            std::cout << label << " min=" << intervals_[0] / 1000.0 - offset
                      << " mean=" << mean - offset << " p50=" << percentile(.50) - offset
                      << " p95=" << percentile(.95) - offset << " p99=" << percentile(.99) - offset
                      << " max=" << intervals_[samples_ - 1] / 1000.0 - offset << '\n';
        };
        row("Interval_us", 0);
        row("Signed_jitter_us(interval-1000)", 1000);
        std::cout << "Observed_span_s=" << static_cast<double>(sum / 1000000000)
                  << " average_hz=" << (sum > 0 ? static_cast<double>(samples_ * 1000000000.0L / sum) : 0)
                  << " intervals_gt_1500us=" << exceedances << '\n'
                  << "Exceedances are period indicators, NOT exact deadline misses; percentiles=nearest-rank.\n";
        std::cout << "Measured_exceedances gt_2ms=" << gt2 << " gt_5ms=" << gt5
                  << " gt_10ms=" << gt10 << " gt_100ms=" << gt100 << '\n';
        if (options_.trace_outliers) ReportTrace();
    }
private:
    struct TraceSample {
        std::int64_t timestamp_ns, interval_ns;
        int cpu;
        bool measured;
    };
    void ReportTrace() const {
        // Ranking/allocation/output only after Runtime::Run has finished.
        std::vector<std::size_t> ranked;
        ranked.reserve(samples_);
        for (std::size_t i = 0; i < trace_count_; ++i)
            if (trace_[i].measured) ranked.push_back(i);
        const auto count = std::min<std::size_t>(20, ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin() + count, ranked.end(),
            [&](std::size_t a, std::size_t b) {
                return trace_[a].interval_ns != trace_[b].interval_ns ?
                    trace_[a].interval_ns > trace_[b].interval_ns : a < b;
            });
        std::cout << "Trace records=" << trace_count_ << " measured=" << ranked.size()
                  << " top=" << count << " window_begin_ns="
                  << first_ + std::int64_t(options_.warmup) * 1000000000LL
                  << " window_end_ns=" << first_ + std::int64_t(options_.duration) * 1000000000LL << '\n';
        std::cout << "Trace clock=CLOCK_MONOTONIC; CPU queried after timestamp; index is zero-based.\n"
                  << "First interval=0 (no predecessor); boundary context may have fewer than 5 neighbors.\n";
        for (std::size_t rank = 0; rank < count; ++rank) {
            const auto center = ranked[rank];
            const auto begin = center > 5 ? center - 5 : 0;
            const auto end = std::min(trace_count_, center + 6);
            std::cout << "Outlier rank=" << rank + 1 << " index=" << center
                      << " interval_ns=" << trace_[center].interval_ns << '\n';
            for (auto i = begin; i < end; ++i) {
                const auto& sample = trace_[i];
                std::cout << "Trace_sample index=" << i << " center=" << (i == center)
                          << " timestamp_ns=" << sample.timestamp_ns << " interval_ns=" << sample.interval_ns
                          << " cpu=" << sample.cpu << " measured=" << sample.measured << '\n';
            }
        }
    }
    Options options_;
    std::vector<std::int64_t> intervals_;
    std::vector<TraceSample> trace_;
    std::size_t samples_{0}, trace_count_{0};
    std::uint64_t loops_{0};
    bool started_{false}, overflow_{false};
    std::int64_t first_{0}, previous_{0};
};

std::size_t Fds() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                                std::filesystem::directory_iterator{}));
}

int Parent(const Options& o, const std::string& executable) {
    struct sigaction action{}; action.sa_handler = Signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) || sigaction(SIGTERM, &action, nullptr))
        throw std::runtime_error("cannot install signal handlers");
    const auto baseline = Fds();
    kcf::Process child;
    std::vector<std::string> args{"--child", "--duration", std::to_string(o.duration),
                                  "--warmup", std::to_string(o.warmup)};
    if (o.trace_outliers) args.push_back("--trace-outliers");
    const int start = child.Start(executable, args, {}, o.profile);
    if (start) {
        std::cerr << "Start error=" << start << " (" << std::strerror(-start)
                  << "); RT policy may require permission. No fallback/retry.\n";
        return 1;
    }
    const pid_t pid = child.GetPid();
    std::cout << "Parent pid=" << getpid() << " child_pid=" << pid
              << " duration_s=" << o.duration << " warmup_s=" << o.warmup << '\n';
    bool stopping = false, pending = false, saw_running = false, completed = false, forced = false;
    int error = 0;
    std::uint64_t request = 0;
    auto next_request = Clock::now(), pending_since = next_request;
    auto deadline = next_request + 5s; // finite startup wait
    auto stop_deadline = deadline;
    // Do not leave a child behind if reporting or an API unexpectedly throws.
    try {
        while (child.IsRunning()) {
            auto now = Clock::now();
            if (!stopping && !pending && now >= next_request) {
                const int code = child.RequestRuntimeStatus(++request);
                if (!code) { pending = true; pending_since = now; }
                else if (code != -EAGAIN && code != -EINTR) error = code;
                next_request = now + 100ms;
            }
            if (!stopping && pending) {
                kcf::RuntimeStatusResponse response;
                const int code = child.ReceiveRuntimeStatus(response);
                if (!code) {
                    pending = false;
                    if (response.runtime_error) error = response.runtime_error;
                    if (response.state == kcf::ProcessState::RUNNING && !saw_running) {
                        saw_running = true; deadline = now + std::chrono::seconds(o.duration);
                    }
                    if (response.state == kcf::ProcessState::ERROR && !error) error = -EFAULT;
                } else if (code != -EAGAIN && code != -EINTR) error = code;
                else if (now - pending_since > 1s) error = -ETIMEDOUT;
            }
            if (!stopping && (interrupted || error || now >= deadline)) {
                completed = saw_running && now >= deadline && !interrupted && !error;
                if (!saw_running && now >= deadline && !error) error = -ETIMEDOUT;
                const int code = child.RequestStop();
                if (code && code != -ESRCH && !error) error = code;
                stopping = true; stop_deadline = now + 5s;
            }
            if (stopping && now >= stop_deadline) { child.ForceStop(); forced = true; break; }
            std::this_thread::sleep_for(5ms);
        }
        child.Wait();
    } catch (...) { child.ForceStop(); child.Wait(); throw; }
    const auto exit = child.GetExitInfo();
    const auto after = Fds();
    std::cout << "Parent reaped=" << (child.GetPid() == -1 && exit.available)
              << " completed=" << completed << " interrupted=" << bool(interrupted)
              << " forced=" << forced << " exit_code=" << exit.exit_code << " signal=" << exit.signal_number
              << " error=" << error << " fd_before=" << baseline << " fd_after=" << after << '\n';
    const bool clean = exit.available && exit.exited_normally && exit.exit_code == 0 &&
        !error && !forced && baseline == after;
    return interrupted ? (clean ? 130 : 1) : (clean && completed ? 0 : 1);
}
} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc == 2 && std::string(argv[1]) == "--help") { Usage(); return 0; }
    Options options;
    try { options = Parse(argc, argv); }
    catch (const std::exception& e) { std::cerr << "Arguments: " << e.what() << '\n'; Usage(); return 2; }
    try {
        if (options.child) {
            Benchmark element(options); // fixed storage allocated before Setup
            kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(1000.0);
            const int code = runtime.Run(element);
            element.Report(code);
            return code ? 1 : 0;
        }
        return Parent(options, std::filesystem::read_symlink("/proc/self/exe").string());
    } catch (const std::exception& e) { std::cerr << "Benchmark error: " << e.what() << '\n'; return 1; }
}
