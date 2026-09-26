#pragma once
#include "mecanum/kinematics.hpp"
#include <cerrno>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

// Test executable only: observe successful UART write calls, without adding
// instrumentation or changing scheduling in the production driver.
namespace uart_trace {
struct Frame { std::uint64_t start_ns, complete_ns; std::string bytes; };
struct Capture {
    struct Pending { std::uint64_t start_ns{}; std::string bytes; };
    std::map<int, Pending> pending;
    std::vector<Frame> motors;
    void Add(int fd, const char* data, std::size_t count, std::uint64_t start, std::uint64_t end) {
        auto& p = pending[fd];
        for (std::size_t i=0; i<count; ++i) {
            const char c=data[i];
            if (c=='\n') continue;
            if (p.bytes.empty()) p.start_ns=start;
            if (c=='\r') {
                if (!p.bytes.empty() && p.bytes.front()=='m') motors.push_back({p.start_ns,end,p.bytes});
                p.bytes.clear();
            } else p.bytes.push_back(c);
        }
    }
};
inline thread_local Capture* active=nullptr;
struct Scope {
    explicit Scope(Capture& capture) { active=&capture; }
    ~Scope() { active=nullptr; }
};
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* bytes, size_t count) {
    const auto start=uart_trace::active ? mecanum::MonotonicNs() : 0;
    const auto result=__real_write(fd,bytes,count);
    const int saved_errno=errno;
    if (uart_trace::active && result>0)
        uart_trace::active->Add(fd,static_cast<const char*>(bytes),result,start,mecanum::MonotonicNs());
    errno=saved_errno;
    return result;
}
