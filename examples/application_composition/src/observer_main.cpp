#include "composition/common.hpp"
#include "kcf/process/process_runtime.hpp"
#include <fcntl.h>

class Observer : public kcf::ProcessElement {
public:
    explicit Observer(std::string instance) : result_(composition::Topic(instance, "result")) {}
    int Setup() override {
        const int code = input_.Open(result_);
        if (code) return code;
        stdin_flags_ = fcntl(STDIN_FILENO, F_GETFL);
        if (stdin_flags_ < 0) {
            if (errno != EBADF) return -errno;
            std::cout << "[Observer] stdin closed; reconnect command unavailable" << std::endl;
        } else {
            if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags_ | O_NONBLOCK) < 0) return -errno;
            restore_stdin_ = true;
            commands_enabled_ = true;
        }
        composition::Started("Observer", 10);
        std::cout << "[Observer] enter r to reconnect explicitly" << std::endl;
        return 0;
    }
    int Loop() override {
        if (ReconnectRequested()) {
            connection_error_ = input_.Reconnect(result_);
            std::cout << "[Observer] reconnect pid=" << getpid() << " code=" << connection_error_
                      << " status=" << (connection_error_ ? "DISCONNECTED" : "WAITING") << std::endl;
        }
        // A failed reconnect invalidates the old snapshot. Stay alive for another explicit r.
        if (!input_.opened) {
            std::cout << "[Observer] status=DISCONNECTED code=" << connection_error_ << std::endl;
            return 0;
        }
        const int read = input_.Read();
        if (read < 0) { std::cerr << "[Observer] status=ERROR code=" << read << std::endl; return read; }
        if (!input_.have) { std::cout << "[Observer] status=WAITING" << std::endl; return 0; }
        const auto& v = input_.latest;
        if (v.source_value != double(v.sequence) * .5 || v.value != v.source_value * 2) return -EBADMSG;
        const auto age = composition::Now() - v.timestamp_ns;
        std::cout << "[Observer] seq=" << v.sequence << " timestamp_ns=" << v.timestamp_ns
                  << " value=" << v.value << " age_ms=" << age/1000000.0 << " status="
                  << (age > 500000000ULL ? "STALE" : read ? "NEW" : "UNCHANGED");
        input_.Print(std::cout); std::cout << std::endl;
        return 0;
    }
    void Shutdown() override {
        const int code = input_.Close(); // never unlink another process's Topic
        int restored = 0;
        if (restore_stdin_) {
            if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags_) < 0) restored = -errno;
            restore_stdin_ = false;
        }
        std::cout << "[Observer] Shutdown"; input_.Print(std::cout);
        std::cout << " close=" << code << " stdin_restore=" << restored << std::endl;
        if (code || restored) throw std::runtime_error("Observer cleanup failed");
    }
private:
    bool ReconnectRequested() {
        if (!commands_enabled_) return false;
        char commands[64]; // bounded work per Loop; no blocking getline or command thread
        const auto size = ::read(STDIN_FILENO, commands, sizeof(commands));
        if (size == 0) { commands_enabled_ = false; return false; }
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::cerr << "[Observer] stdin error=" << -errno << std::endl;
                commands_enabled_ = false;
            }
            return false;
        }
        // Single-character commands; coalesce r's in this batch into one attempt.
        for (ssize_t i = 0; i < size; ++i) if (commands[i] == 'r') return true;
        return false;
    }
    int stdin_flags_{-1}, connection_error_{0};
    bool restore_stdin_{false}, commands_enabled_{false};
    std::string result_;
    composition::Input<composition::Result> input_;
};
int main(int argc, char** argv) {
    try {
        const auto instance = composition::Instance(argc, argv);
        if (kcf::DetectLaunchExecutionMode() != kcf::ExecutionMode::STANDALONE)
            throw std::invalid_argument("Observer must be launched independently, outside Bringup");
        Observer element(instance);
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(10);
        const int code = runtime.Run(element);
        if (code) std::cerr << "[Observer] runtime_error=" << code << std::endl;
        return code ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
