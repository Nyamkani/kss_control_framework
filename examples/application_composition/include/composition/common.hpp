#pragma once
#include "kcf/ipc/subscriber.hpp"
#include "kcf/process/execution_mode.hpp"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <type_traits>
#include <unistd.h>

namespace composition {
struct Sample { std::uint64_t sequence, timestamp_ns; double value; };
struct Result { std::uint64_t sequence, timestamp_ns; double source_value, value; };
static_assert(std::is_trivial_v<Sample> && std::is_trivially_copyable_v<Sample>);
static_assert(std::is_trivial_v<Result> && std::is_trivially_copyable_v<Result>);
static_assert(std::is_standard_layout_v<Sample> && std::is_standard_layout_v<Result>);

inline std::string Instance(int argc, char** argv) {
    if (argc != 2) throw std::invalid_argument("usage: EXECUTABLE INSTANCE (same unique instance for all four programs)");
    const std::string name = argv[1];
    if (name.empty() || name.size() > 40 || name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        throw std::invalid_argument("INSTANCE must contain 1..40 letters, digits, '_' or '-'");
    return name;
}
inline std::string Topic(const std::string& instance, const char* suffix) {
    return "/kcf_composition_" + instance + "_" + suffix;
}
inline std::uint64_t Now() {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t)) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return std::uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
inline void Started(const char* role, unsigned hz) {
    std::cout << '[' << role << "] pid=" << getpid() << " mode="
        << (kcf::DetectLaunchExecutionMode() == kcf::ExecutionMode::STANDALONE ? "Standalone" : "Supervised")
        << " hz=" << hz << std::endl;
}
// Pull-only Subscriber: each Element owns its reads; no callback/shared-state race.
// Same transport sequence is a normal repeated snapshot, not a duplicate publication.
template<class T> struct Input {
    kcf::Subscriber<T> subscriber;
    T latest{};
    bool opened{false}, have{false};
    std::uint64_t transport{0}, received{0}, repeated{0}, skipped{0}, duplicates{0}, reversed{0};
    int Open(const std::string& name) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int code;
        do {
            code = subscriber.Create(name);
            if (code != -ENOENT && code != -EAGAIN) { opened = code == 0; return code; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        return -ETIMEDOUT;
    }
    // Explicit reconnect only: never carry comparison state across SHM generations.
    // One Create attempt; the caller decides when another user request is made.
    int Reconnect(const std::string& name) {
        const int closed = Close();
        latest = T{};
        have = false;
        transport = received = repeated = skipped = duplicates = reversed = 0;
        if (closed) return closed;
        const int code = subscriber.Create(name);
        opened = code == 0;
        return code;
    }
    // 1: new valid snapshot, 0: repeated/no data, negative: fatal API/payload error.
    int Read() {
        T value{}; kcf::TopicReadInfo info{};
        const int code = subscriber.ReadLatest(value, info);
        if (code == -EAGAIN) return 0;
        if (code) return code;
        if (have && info.sequence == transport) { ++repeated; return 0; }
        if (!value.sequence || !value.timestamp_ns || value.timestamp_ns > Now()) return -EBADMSG;
        if (have) {
            if (value.sequence == latest.sequence) { ++duplicates; return -EBADMSG; }
            if (value.sequence < latest.sequence || value.timestamp_ns < latest.timestamp_ns || info.sequence < transport) {
                ++reversed; return -EBADMSG;
            }
            skipped += value.sequence - latest.sequence - 1;
        }
        latest = value; transport = info.sequence; have = true; ++received;
        return 1;
    }
    int Close() { if (!opened) return 0; opened = false; return subscriber.Close(); }
    void Print(std::ostream& out) const {
        out << " received=" << received << " repeated=" << repeated << " skipped=" << skipped
            << " duplicates=" << duplicates << " reversed=" << reversed;
    }
};
} // namespace composition
