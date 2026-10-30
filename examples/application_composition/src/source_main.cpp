#include "composition/common.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_runtime.hpp"
#include <limits>

class Source : public kcf::ProcessElement {
public:
    explicit Source(std::string instance) : topic_(composition::Topic(instance, "source")) {}
    int Setup() override {
        const int code = publisher_.Create(topic_); owned_ = code == 0;
        if (!code) composition::Started("Source", 100);
        return code;
    }
    int Loop() override {
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
        const composition::Sample value{sequence_ + 1, composition::Now(), double(sequence_ + 1) * .5};
        const int code = publisher_.Publish(value);
        if (code == -EAGAIN) { ++busy_; return 0; }
        if (code) return code;
        ++sequence_;
        if (value.timestamp_ns - last_log_ >= 1000000000ULL) {
            last_log_ = value.timestamp_ns;
            std::cout << "[Source] seq=" << sequence_ << " published=" << sequence_ << " eagain=" << busy_ << std::endl;
        }
        return 0;
    }
    void Shutdown() override {
        int close = 0, unlink = 0;
        if (owned_) { close = publisher_.Close(); unlink = publisher_.Unlink(); owned_ = false; }
        std::cout << "[Source] Shutdown published=" << sequence_ << " eagain=" << busy_
                  << " close=" << close << " unlink=" << unlink << std::endl;
        if (close || unlink) throw std::runtime_error("Source cleanup failed");
    }
private:
    std::string topic_;
    kcf::Publisher<composition::Sample> publisher_;
    bool owned_{false};
    std::uint64_t sequence_{0}, busy_{0}, last_log_{0};
};
int main(int argc, char** argv) {
    try {
        Source element(composition::Instance(argc, argv));
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(100);
        const int code = runtime.Run(element);
        if (code) std::cerr << "[Source] runtime_error=" << code << std::endl;
        return code ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
