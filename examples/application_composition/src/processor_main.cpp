#include "composition/common.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/process/process_runtime.hpp"

class Processor : public kcf::ProcessElement {
public:
    explicit Processor(std::string instance) : source_(composition::Topic(instance, "source")),
        result_(composition::Topic(instance, "result")) {}
    int Setup() override {
        int code = input_.Open(source_);
        if (code) return code;
        code = publisher_.Create(result_); owned_ = code == 0;
        if (!code) composition::Started("Processor", 50);
        return code;
    }
    int Loop() override {
        const int read = input_.Read();
        if (read <= 0) return read;
        const auto& v = input_.latest;
        if (v.value != double(v.sequence) * .5) return -EBADMSG;
        const composition::Result value{v.sequence, v.timestamp_ns, v.value, v.value * 2};
        const int code = publisher_.Publish(value);
        if (code == -EAGAIN) { ++busy_; return 0; }
        if (code) return code;
        ++published_;
        const auto now = composition::Now();
        if (now - last_log_ >= 1000000000ULL) {
            last_log_ = now;
            std::cout << "[Processor] seq=" << v.sequence << " age_ms=" << (now-v.timestamp_ns)/1000000.0
                      << " published=" << published_;
            input_.Print(std::cout); std::cout << std::endl;
        }
        return 0;
    }
    void Shutdown() override {
        const int input_close = input_.Close();
        int close = 0, unlink = 0;
        if (owned_) { close = publisher_.Close(); unlink = publisher_.Unlink(); owned_ = false; }
        std::cout << "[Processor] Shutdown published=" << published_ << " eagain=" << busy_;
        input_.Print(std::cout);
        std::cout << " input_close=" << input_close << " close=" << close << " unlink=" << unlink << std::endl;
        if (input_close || close || unlink) throw std::runtime_error("Processor cleanup failed");
    }
private:
    std::string source_, result_;
    composition::Input<composition::Sample> input_;
    kcf::Publisher<composition::Result> publisher_;
    bool owned_{false};
    std::uint64_t published_{0}, busy_{0}, last_log_{0};
};
int main(int argc, char** argv) {
    try {
        Processor element(composition::Instance(argc, argv));
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(50);
        const int code = runtime.Run(element);
        if (code) std::cerr << "[Processor] runtime_error=" << code << std::endl;
        return code ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
