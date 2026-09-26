#include "mecanum/motor_element.hpp"
#include "uart_write_trace.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <poll.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mecanum;
using namespace std::chrono_literals;
namespace mecanum {
struct MotorElementTestAccess {
    static void Observe(MotorElement& motor, std::vector<MotorCommandObservation>& records) {
        motor.observer_context_=&records;
        motor.command_observer_=[](void* context, const MotorCommandObservation& record) {
            static_cast<std::vector<MotorCommandObservation>*>(context)->push_back(record);
        };
    }
};
}
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool Near(double a, double b, double tolerance = 1e-8) { return std::abs(a - b) < tolerance; }

// Only a PTY is opened: this emulator has no path to physical motor hardware.
class Arduino {
public:
    struct Event { char command; bool zero; std::uint64_t timestamp_ns; float first_rpm{0}; };
    std::vector<Event> Events() {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }
    Arduino() {
        master_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        Check(master_ >= 0, "posix_openpt");
        Check(grantpt(master_) == 0 && unlockpt(master_) == 0, "PTY setup");
        device = ptsname(master_);
        worker_ = std::thread([this] { Run(); });
    }
    ~Arduino() {
        running_ = false;
        worker_.join();
        close(master_);
    }
    std::array<float, 4> LastTarget() {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_;
    }
    unsigned MotionCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return motion_count_;
    }
    std::string device;
    std::atomic<bool> malformed_encoder{false}, no_encoder_reply{false}, no_stop_ack{false},
                      no_rpm_reply{false};
    std::atomic<unsigned> delay_encoder_ms{0}, delay_motor_ms{0}, delay_rpm_ms{0};
    std::atomic<unsigned> continuous_delay_ms{0};
    // Fault injection applies only to motion. The previous ACK is released
    // when the next zero command arrives, AFTER the host's input flush.
    enum class MotionReply { Normal, Oversized, Malformed, Timeout };
    std::atomic<MotionReply> motion_reply{MotionReply::Normal};
    std::atomic<unsigned> delayed_acks{0}, received_bytes{0};
    std::atomic<bool> boot_before_ack{false};
    std::atomic<bool> strict_motor_validation{false};
    void Inject(const std::string& frame) { Reply(frame); }
private:
    void Reply(const std::string& text) {
        // Split a frame to exercise accumulation rather than one-read assumptions.
        for (char c : text) {
            while (running_) {
                if (write(master_, &c, 1) == 1) break;
                std::this_thread::sleep_for(1ms);
            }
        }
    }
    void Handle(const std::string& line) {
        std::istringstream in(line);
        char command = 0;
        in >> command;
        if (command != 'm') {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back({command, false, MonotonicNs()});
        }
        if (command == 'm') {
            std::array<float, 4> rpm{};
            for (float& value : rpm) in >> value;
            if (strict_motor_validation) {
                bool valid = !in.fail();
                std::string extra;
                valid = valid && !(in >> extra);
                for (const auto value : rpm) valid &= std::isfinite(value) && std::abs(value) <= 120;
                if (!valid) { Reply("Invalid Command\r\n"); return; }
            }
            bool zero = true;
            for (float value : rpm) zero = zero && value == 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                target_ = rpm;
                if (!zero) ++motion_count_;
                events_.push_back({command, zero, MonotonicNs(), rpm[0]});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_motor_ms.exchange(0)));
            std::this_thread::sleep_for(std::chrono::milliseconds(continuous_delay_ms.load()));
            if (!zero) {
                const auto reply = motion_reply.load();
                if (reply != MotionReply::Normal) {
                    pending_old_ack_ = true;
                    if (reply == MotionReply::Oversized) Reply(std::string(128, 'X'));
                    else if (reply == MotionReply::Malformed) Reply("BAD ACK\r\n");
                    return;
                }
            }
            if (zero && pending_old_ack_) {
                Reply("OK\r\n"); // ACK belongs to the previous motion, not this stop
                pending_old_ack_ = false;
                ++delayed_acks;
            }
            if (boot_before_ack.exchange(false)) Reply("Setup Complete!\r\n");
            if (!(zero && no_stop_ack)) Reply("OK\r\n");
        } else if (command == 'r') Reply("OK\r\n");
        else if (command == 'e') {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_encoder_ms.exchange(0)));
            std::this_thread::sleep_for(std::chrono::milliseconds(continuous_delay_ms.load()));
            if (!no_encoder_reply) Reply(malformed_encoder ? "1 2 3 4 extra\r\n" : "11 22 33 44\r\n");
        } else if (command == 'z') {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_rpm_ms.exchange(0)));
            std::this_thread::sleep_for(std::chrono::milliseconds(continuous_delay_ms.load()));
            if (!no_rpm_reply) Reply("1.25 -2.5 3.75 -4.5\r\n");
        }
    }
    void Run() {
        std::string frame;
        while (running_) {
            pollfd p{master_, POLLIN, 0};
            if (poll(&p, 1, 5) <= 0) continue;
            char c;
            if (read(master_, &c, 1) != 1) { std::this_thread::sleep_for(1ms); continue; }
            ++received_bytes;
            if (c == '\r') { if (!frame.empty()) Handle(frame); frame.clear(); }
            else if (c != '\n') frame.push_back(c);
        }
    }
    int master_{-1};
    std::atomic<bool> running_{true};
    std::thread worker_;
    std::mutex mutex_;
    std::array<float, 4> target_{};
    unsigned motion_count_{0};
    bool pending_old_ack_{false}; // emulator worker only
    std::vector<Event> events_;
};

std::string Prefix() {
    static unsigned next = 0;
    return "/mecanum_test_" + std::to_string(getpid()) + "_" + std::to_string(++next);
}
struct CommandPublisher {
    kcf::Publisher<CmdVel> pub;
    bool owned{false};
    ~CommandPublisher() { pub.Close(); if (owned) pub.Unlink(); }
    void Create(const std::string& prefix, unsigned depth = 1) {
        Check(pub.Create(prefix + "/cmd_vel", depth) == 0, "create command");
        owned = true;
    }
};
struct ElementGuard {
    MotorElement& element;
    bool closed{false};
    ~ElementGuard() { if (!closed) { try { element.Shutdown(); } catch (...) {} } }
    bool Close() {
        closed = true;
        try { element.Shutdown(); return true; } catch (...) { return false; }
    }
};
MotorOptions Options(const Arduino& arduino, const std::string& prefix) {
    MotorOptions options;
    options.device = arduino.device;
    options.topic_prefix = prefix;
    options.boot_wait_ms = 0; // emulator only; executable retains original boot delay
    options.command_timeout_ns = 60000000;
    return options;
}
void Publish(CommandPublisher& owner, CmdVel command) {
    Check(owner.pub.Publish(command) == 0, "publish command");
    // Allow callback worker scheduling; correctness observations use real Topic output.
    std::this_thread::sleep_for(8ms);
}
WheelState ReadWheels(kcf::Subscriber<WheelState>& reader) {
    WheelState result{};
    kcf::TopicReadInfo info{};
    Check(reader.ReadLatest(result, info) == 0, "read wheels");
    return result;
}
void MathAndContracts() {
    Check(EncoderDelta(INT32_MIN + 7, INT32_MAX - 8) == 16, "positive encoder wrap");
    Check(EncoderDelta(INT32_MAX - 8, INT32_MIN + 7) == -16, "negative encoder wrap");
    CmdVel c{1, 100, 1, 0, 0};
    Check(CommandFresh(c, 120, 90, 30), "fresh command");
    Check(!CommandFresh(c, 130, 90, 30), "expiry boundary");
    Check(!CommandFresh(c, 99, 90, 30), "future command");
    Check(!CommandFresh(c, 120, 101, 30), "previous lifecycle command");
    c.vx_m_s = std::numeric_limits<double>::quiet_NaN();
    Check(!CommandFresh(c, 120, 90, 30), "NaN command");
    c = {1, 100, 1, 0, 0};
    WheelRpm rpm{};
    Check(InverseKinematics(c, rpm), "inverse kinematics");
    for (float r : rpm) Check(Near(r, -60.0 / (2 * PI * 0.041), 0.0001), "forward RPM");
    c = {1, 100, 0, 1, 0};
    Check(InverseKinematics(c, rpm) && rpm[0] > 0 && rpm[1] < 0 &&
          rpm[2] < 0 && rpm[3] > 0, "lateral signs");
    c = {1, 100, 0, 0, 1};
    Check(InverseKinematics(c, rpm) && rpm[0] > 0 && rpm[1] < 0 &&
          rpm[2] > 0 && rpm[3] < 0, "yaw signs");
    Odometry odom;
    odom.Initialize({}, 1000000000);
    auto s = odom.Update({-4320, -4320, -4320, -4320}, 2000000000);
    Check(s.valid && Near(s.x_m, 2 * PI * 0.041) && Near(s.y_m, 0) &&
          Near(s.vx_m_s, 2 * PI * 0.041), "one revolution forward");
    s = odom.Update({1000000, 1000000, 1000000, 1000000}, 2015000000);
    Check(!s.valid && !s.resynchronized, "first bad delta rejected");
    odom.Update({1000000, 1000000, 1000000, 1000000}, 2030000000);
    s = odom.Update({1000000, 1000000, 1000000, 1000000}, 2045000000);
    Check(!s.valid && s.resynchronized && Near(s.x_m, 2 * PI * 0.041), "resync without pose jump");
    s = odom.Update({1000000, 1000000, 1000000, 1000000}, 2060000000);
    Check(s.valid && Near(s.vx_m_s, 0), "recovered baseline");
    odom.Initialize({}, 1000000000);
    s = odom.Update({4320, -4320, -4320, 4320}, 2000000000);
    Check(s.valid && Near(s.x_m, 0) && Near(s.y_m, 2 * PI * 0.041), "lateral odometry");
    odom.Initialize({}, 1000000000);
    s = odom.Update({4320, -4320, 4320, -4320}, 2000000000);
    Check(s.valid && Near(s.yaw_rad, (2 * PI * 0.041) / 0.1975), "yaw odometry");
    Check(kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<CmdVel>::Get()), "command descriptor");
    Check(kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<WheelState>::Get()), "wheel descriptor");
    Check(kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<WheelOdometry>::Get()), "odom descriptor");
}
void StartupFrames() {
    {
        Arduino arduino;
        auto options = Options(arduino, Prefix());
        options.boot_wait_ms = 100;
        MotorElement motor(options);
        ElementGuard guard{motor};
        // Banner arrives during the boot wait, after Open's input flush.
        std::thread boot([&] {
            std::this_thread::sleep_for(30ms);
            arduino.Inject("Setup Com");
            std::this_thread::sleep_for(5ms);
            arduino.Inject("plete!\r\n");
        });
        const int result = motor.Setup();
        boot.join();
        Check(result == 0 && guard.Close(), "split boot banner during Setup preserves command ACKs");
    }
    for (const std::string frame : {"", "Setup Complete!\r\n", "\r\nSetup Complete!\r\n",
                                    "OK\r\n", "Unknown boot\r\n", "Setup Complete!\r",
                                    "Setup Complete!\n"}) {
        Arduino arduino;
        ArduinoMotorDriver driver;
        Check(driver.Initialize(arduino.device, 115200) == 0, "startup open");
        arduino.Inject(frame);
        const bool valid = frame.empty() || frame == "Setup Complete!\r\n" ||
                           frame == "\r\nSetup Complete!\r\n";
        Check((driver.PrepareStartup() == 0) == valid, "startup accepts only complete known lines");
        Check(arduino.received_bytes == 0, "startup does not send a command");
        if (valid) {
            Check(driver.WriteRPM({}).error == 0, "first command retains its own ACK");
            Check(driver.ResetEncoders() == 0, "subsequent ACK remains aligned");
        }
    }
    {
        Arduino arduino;
        ArduinoMotorDriver driver;
        Check(driver.Initialize(arduino.device, 115200) == 0, "late boot open");
        Check(driver.PrepareStartup() == 0, "silent startup");
        arduino.boot_before_ack = true;
        const auto result = driver.WriteRPM({});
        Check(result.error == -EPROTO && result.phase == SendPhase::SendAttempted,
              "boot after send is uncertain, never skipped to find OK");
        Check(driver.BestEffortStop() != 0, "late boot stop remains unconfirmed");
    }
    {
        Arduino arduino;
        MotorElement motor(Options(arduino, Prefix()));
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "startup integrated setup");
        arduino.boot_before_ack = true;
        // Force a refresh even with no producer; UART error must latch in Element.
        std::this_thread::sleep_for(105ms);
        Check(motor.Loop() == -EPROTO, "reboot during transaction faults Element");
        Check(!guard.Close(), "reboot fault survives Shutdown ACK");
    }
}
void DriverProtocol() {
    Arduino arduino;
    ArduinoMotorDriver driver;
    Check(driver.Initialize(arduino.device, 115200) == 0, "UART open");
    Check(driver.ResetEncoders() == 0, "r ACK");
    const auto normal = driver.WriteRPM({1, 2, 3, 4});
    Check(normal.error == 0 && normal.phase == SendPhase::SendAttempted, "m ACK");
    Check(arduino.LastTarget() == WheelRpm{1, 3, 4, 2}, "wire motor mapping");
    Encoders enc{};
    Check(driver.ReadEncoders(enc) == 0 && enc == Encoders{11, 44, 22, 33}, "encoder mapping");
    WheelRpm rpm{};
    Check(driver.ReadRPM(rpm) == 0 && rpm == WheelRpm{1.25f, -4.5f, -2.5f, 3.75f}, "RPM mapping");
    const auto expired = driver.WriteRPM({1, 1, 1, 1}, MonotonicNs() - 1);
    Check(expired.error == -ETIME && expired.phase == SendPhase::NotStarted, "expired send rejected");
    Check(arduino.MotionCount() == 1, "expired command never on wire");
    const auto bytes_before = arduino.received_bytes.load();
    const auto oversized = driver.WriteRPM({1e20f, 1e20f, 1e20f, 1e20f});
    Check(oversized.error == -EMSGSIZE && oversized.phase == SendPhase::NotStarted,
          "oversized command classified before send");
    // Successful subsequent query is a serial barrier: only its three bytes
    // may have reached the emulator after local size rejection.
    Check(driver.ReadEncoders(enc) == 0, "query after pre-send rejection");
    Check(arduino.received_bytes == bytes_before + 3 && arduino.MotionCount() == 1,
          "pre-send size rejection writes no UART bytes");
    arduino.malformed_encoder = true;
    Check(driver.ReadEncoders(enc) == -EPROTO && enc == Encoders{11, 44, 22, 33},
          "bad frame does not partially commit");
    arduino.no_encoder_reply = true;
    const auto start = MonotonicNs();
    Check(driver.ReadEncoders(enc) == -ETIMEDOUT, "missing reply timeout");
    Check(MonotonicNs() - start < 500000000ULL, "bounded reply wait");
    Check(driver.BestEffortStop() != 0, "ambiguous stop never claims success");
}
void CommandErrorClassification() {
    {
        Arduino arduino;
        const auto prefix = Prefix();
        CommandPublisher input;
        input.Create(prefix);
        MotorElement motor(Options(arduino, prefix));
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "pre-send rejection setup");
        kcf::Subscriber<WheelState> reader;
        Check(reader.Create(prefix + "/wheel_state") == 0, "pre-send rejection reader");
        Publish(input, {1, MonotonicNs(), 1e20, 0, 0});
        Check(motor.Loop() == 0 && arduino.MotionCount() == 0,
              "pre-send size rejection retains zero fallback policy");
        const auto state = ReadWheels(reader);
        Check(!state.command_fresh && state.command_acknowledged && state.last_error == 0,
              "local rejection is not a protocol fault");
        Check(guard.Close(), "normal stop after pre-send rejection");
    }
    for (const auto reply : {Arduino::MotionReply::Oversized,
                            Arduino::MotionReply::Malformed, Arduino::MotionReply::Timeout}) {
        Arduino arduino;
        const auto prefix = Prefix();
        CommandPublisher input;
        input.Create(prefix);
        MotorElement motor(Options(arduino, prefix));
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "post-send error setup");
        kcf::Subscriber<WheelState> reader;
        Check(reader.Create(prefix + "/wheel_state") == 0, "post-send error reader");
        arduino.motion_reply = reply;
        arduino.no_stop_ack = true; // any following OK can ONLY be the old ACK
        Publish(input, {1, MonotonicNs(), 0.1, 0, 0});
        const int expected = reply == Arduino::MotionReply::Oversized ? -EMSGSIZE :
            reply == Arduino::MotionReply::Malformed ? -EPROTO : -ETIMEDOUT;
        Check(motor.Loop() == expected && arduino.MotionCount() == 1,
              "post-send error is fatal, not a pre-send rejection");
        auto state = ReadWheels(reader);
        Check(state.last_error == expected && !state.command_acknowledged &&
              !state.command_fresh, "post-send fault telemetry");
        Check(!guard.Close() && motor.ShutdownError() != 0,
              "delayed previous ACK cannot confirm shutdown");
        state = ReadWheels(reader); // existing mapping retains the final snapshot
        Check(!state.command_acknowledged && state.last_error != 0,
              "final stop remains unconfirmed after old ACK");
        const auto deadline = MonotonicNs() + 500000000ULL;
        while (!arduino.delayed_acks && MonotonicNs() < deadline)
            std::this_thread::sleep_for(1ms);
        Check(arduino.delayed_acks == 1 && arduino.LastTarget() == WheelRpm{},
              "old ACK injected only after subsequent stop was received");
    }
    std::cout << "PASS: pre-send rejection, oversized/malformed/timeout replies and delayed old ACK\n";
}
void ElementLifecycle() {
    Arduino arduino;
    const auto prefix = Prefix();
    CommandPublisher input;
    input.Create(prefix);
    // A latched command from before Setup must not move the robot.
    Publish(input, {1, MonotonicNs(), 0.1, 0, 0});
    MotorElement motor(Options(arduino, prefix));
    ElementGuard guard{motor};
    Check(motor.Setup() == 0, "element setup");
    kcf::Subscriber<WheelState> reader;
    Check(reader.Create(prefix + "/wheel_state") == 0 && reader.GetDepth() == 1, "wheel topic depth");
    kcf::Subscriber<WheelOdometry> odom_reader;
    Check(odom_reader.Create(prefix + "/wheel_odometry") == 0 && odom_reader.GetDepth() == 1, "odom topic depth");
    std::this_thread::sleep_for(8ms);
    Check(motor.Loop() == 0 && arduino.MotionCount() == 0, "startup ignores retained command");
    Publish(input, {2, MonotonicNs(), 0.1, 0, 0});
    Check(motor.Loop() == 0, "fresh loop");
    auto state = ReadWheels(reader);
    Check(state.command_fresh && state.command_acknowledged && state.target_rpm[0] < 0, "fresh command executed");
    Check(motor.Loop() == 0 && motor.Loop() == 0, "distributed sensor slots");
    state = ReadWheels(reader);
    Check(state.encoder_counts[1] == 44 && state.measured_rpm[1] == -4.5f &&
          state.encoder_valid && state.rpm_valid, "wheel telemetry");
    Check(state.rpm_timestamp_ns != 0 && state.encoder_timestamp_ns != 0, "independent sample times");
    std::this_thread::sleep_for(65ms);
    Check(motor.Loop() == 0, "expired loop");
    state = ReadWheels(reader);
    Check(!state.command_fresh && state.command_acknowledged && arduino.LastTarget() == WheelRpm{},
          "expiry commands zero");
    const auto before = arduino.MotionCount();
    Publish(input, {3, MonotonicNs() + 1000000000ULL, 0.1, 0, 0});
    Check(motor.Loop() == 0 && arduino.MotionCount() == before, "future command not executed");
    Publish(input, {4, MonotonicNs(), std::numeric_limits<double>::infinity(), 0, 0});
    Check(motor.Loop() == 0 && arduino.MotionCount() == before, "nonfinite command not executed");
    // Freeze the lifecycle briefly while several callbacks arrive: latest zero wins.
    Publish(input, {5, MonotonicNs(), 0.1, 0, 0});
    Publish(input, {6, MonotonicNs(), 0, 0, 0});
    Check(motor.Loop() == 0 && arduino.MotionCount() == before, "latest command only");
    Publish(input, {7, MonotonicNs(), 0.1, 0, 0});
    Check(motor.Loop() == 0, "motion slot before delayed sensor");
    if (arduino.LastTarget() == WheelRpm{})
        Check(motor.Loop() == 0, "motion after reserved sensor slot");
    Check(arduino.LastTarget() != WheelRpm{}, "motion established before delayed sensor");
    arduino.delay_encoder_ms = 75;
    arduino.delay_rpm_ms = 75;
    Check(motor.Loop() == 0 && arduino.LastTarget() == WheelRpm{}, "expiry during sensor read stops");
    arduino.delay_encoder_ms = arduino.delay_rpm_ms = 0;
    Publish(input, {8, MonotonicNs(), 0.1, 0, 0});
    arduino.delay_motor_ms = 75;
    Check(motor.Loop() == 0 && arduino.LastTarget() == WheelRpm{}, "expiry during ACK stops");
    Publish(input, {9, MonotonicNs(), 0.1, 0, 0});
    Check(motor.Loop() == 0, "motion before shutdown");
    if (arduino.LastTarget() == WheelRpm{}) Check(motor.Loop() == 0, "motion after sensor before shutdown");
    Check(arduino.LastTarget() != WheelRpm{}, "moving before shutdown");
    Check(guard.Close() && arduino.LastTarget() == WheelRpm{}, "shutdown zero ACK");
    kcf::Subscriber<WheelState> after;
    Check(after.Create(prefix + "/wheel_state") == -ENOENT, "owned topic removed");
}
void FailureCases() {
    {
        Arduino arduino;
        const auto prefix = Prefix();
        MotorElement motor(Options(arduino, prefix));
        ElementGuard guard{motor};
        Check(motor.Setup() == 0 && motor.Loop() == 0 && arduino.MotionCount() == 0, "no producer remains stopped");
        CommandPublisher input;
        input.Create(prefix);
        Check(motor.Loop() == 0, "late producer attach");
        Publish(input, {1, MonotonicNs(), 0.1, 0, 0});
        Check(motor.Loop() == 0 && arduino.MotionCount() > 0, "late producer command");
        arduino.no_stop_ack = true;
        Check(!guard.Close() && motor.ShutdownError() != 0, "shutdown missing ACK is failure");
    }
    {
        Arduino arduino;
        const auto prefix = Prefix();
        MotorElement motor(Options(arduino, prefix));
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "fault case setup");
        arduino.no_encoder_reply = true;
        Check(motor.Loop() == -ETIMEDOUT, "UART failure ends lifecycle");
        Check(!guard.Close() && motor.ShutdownError() != 0, "fault cleanup cannot confirm stop");
    }
    {
        Arduino arduino;
        arduino.no_stop_ack = true;
        MotorElement motor(Options(arduino, Prefix()));
        ElementGuard guard{motor};
        Check(motor.Setup() == -ETIMEDOUT, "setup requires zero ACK");
        Check(!guard.Close(), "partial setup stop unconfirmed");
    }
    {
        Arduino arduino;
        const auto prefix = Prefix();
        CommandPublisher input;
        input.Create(prefix, 2);
        MotorElement motor(Options(arduino, prefix));
        ElementGuard guard{motor};
        Check(motor.Setup() == -EINVAL && arduino.MotionCount() == 0, "reject input depth other than one");
        Check(guard.Close(), "depth failure cleanup");
    }
}
void SleepUntil(std::uint64_t time_ns) {
    while (MonotonicNs() < time_ns)
        std::this_thread::sleep_for(1ms);
}
// Timestamp the first diagnostic byte without delaying or moving UART work.
class DiagnosticProbe : public std::streambuf {
public:
    DiagnosticProbe() : previous_(std::cerr.rdbuf(this)) {}
    ~DiagnosticProbe() { std::cerr.rdbuf(previous_); }
    std::uint64_t first_ns{0};
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (!first_ns) first_ns = MonotonicNs();
        return previous_->sputn(data, size);
    }
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        if (!first_ns) first_ns = MonotonicNs();
        return previous_->sputc(traits_type::to_char_type(c));
    }
private:
    std::streambuf* previous_;
};
void DistributedTelemetry() {
    Arduino arduino;
    const auto prefix = Prefix();
    CommandPublisher input;
    input.Create(prefix);
    auto options = Options(arduino, prefix);
    options.command_timeout_ns = 250000000ULL;
    MotorElement motor(options);
    ElementGuard guard{motor};
    Check(motor.Setup() == 0, "distributed telemetry setup");
    kcf::Subscriber<WheelState> reader;
    kcf::Subscriber<WheelOdometry> odom_reader;
    Check(reader.Create(prefix + "/wheel_state") == 0, "telemetry reader");
    Check(odom_reader.Create(prefix + "/wheel_odometry") == 0, "odometry reader");
    auto read_odom = [&] {
        WheelOdometry value{};
        kcf::TopicReadInfo info{};
        Check(odom_reader.ReadLatest(value, info) == 0, "odometry snapshot");
        return value;
    };
    const auto initial = ReadWheels(reader);
    const auto initial_odom = read_odom();
    auto first = arduino.Events().size();
    Publish(input, {1, MonotonicNs(), 0.1, 0, 0});
    Check(motor.Loop() == 0, "command-only slot");
    auto events = arduino.Events();
    Check(events.size() == first + 1 && events.back().command == 'm', "no sensors after command");
    auto state = ReadWheels(reader);
    Check(state.encoder_timestamp_ns == initial.encoder_timestamp_ns &&
          state.rpm_timestamp_ns == initial.rpm_timestamp_ns &&
          state.encoder_valid == initial.encoder_valid && state.rpm_valid == initial.rpm_valid,
          "command slot preserves measurement time and validity");
    Check(read_odom().sequence == initial_odom.sequence &&
          read_odom().timestamp_ns == initial_odom.timestamp_ns, "no invented odometry after command");
    first = events.size();
    Check(motor.Loop() == 0, "encoder slot");
    events = arduino.Events();
    Check(events.size() == first + 1 && events.back().command == 'e', "one encoder transaction");
    state = ReadWheels(reader);
    Check(state.encoder_timestamp_ns > initial.encoder_timestamp_ns &&
          state.rpm_timestamp_ns == initial.rpm_timestamp_ns && !state.rpm_valid,
          "encoder slot does not refresh RPM");
    const auto encoder_state = state;
    const auto encoder_odom = read_odom();
    Check(encoder_odom.sequence > initial_odom.sequence && encoder_odom.valid, "encoder updates odometry");
    first = events.size();
    Check(motor.Loop() == 0, "rpm slot");
    events = arduino.Events();
    state = ReadWheels(reader);
    Check(events.size() == first + 1 && events.back().command == 'z', "one RPM transaction");
    Check(state.encoder_timestamp_ns == encoder_state.encoder_timestamp_ns &&
          state.rpm_timestamp_ns > encoder_state.rpm_timestamp_ns && state.rpm_valid,
          "RPM slot preserves encoder time");
    Check(read_odom().sequence == encoder_odom.sequence &&
          read_odom().timestamp_ns == encoder_odom.timestamp_ns, "RPM does not republish new odometry");
    Check(guard.Close(), "telemetry shutdown");
}
void StopOrderAndLatency() {
    enum class Scenario { NormalStop, DelayedStopAck, ExpiredAtEntry, EncoderExpiry,
                          RpmExpiry, EncoderFault, RpmFault };
    for (const auto scenario : {Scenario::NormalStop, Scenario::DelayedStopAck,
            Scenario::ExpiredAtEntry, Scenario::EncoderExpiry, Scenario::RpmExpiry,
            Scenario::EncoderFault, Scenario::RpmFault}) {
        Arduino arduino;
        const auto prefix = Prefix();
        CommandPublisher input;
        input.Create(prefix);
        auto options = Options(arduino, prefix);
        options.command_timeout_ns = 250000000ULL; // production default, not the short legacy fixture
        MotorElement motor(options);
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "timing setup");
        kcf::Subscriber<WheelState> reader;
        Check(reader.Create(prefix + "/wheel_state") == 0, "timing telemetry");
        const auto generated = MonotonicNs();
        Publish(input, {1, generated, 0.1, 0, 0});
        Check(motor.Loop() == 0 && arduino.MotionCount() == 1, "timing motion established");
        const bool rpm = scenario == Scenario::RpmExpiry || scenario == Scenario::RpmFault;
        const bool sensor_expiry = scenario == Scenario::EncoderExpiry || scenario == Scenario::RpmExpiry;
        const bool fault = scenario == Scenario::EncoderFault || scenario == Scenario::RpmFault;
        if (rpm) Check(motor.Loop() == 0, "encoder before RPM scenario");
        else Check(motor.Loop() == 0 && motor.Loop() == 0,
                   "complete encoder/RPM slots before next encoder scenario");
        std::uint64_t reference = 0;
        const char* label = "";
        if (sensor_expiry) {
            // Refresh the same target at age 180 ms, then start a 75 ms query
            // at age 220 ms. The unchanged 250 ms deadline expires in that query.
            SleepUntil(generated + 180000000ULL);
            Check(motor.Loop() == 0, "refresh target before delayed query");
            SleepUntil(generated + 220000000ULL);
            Check(MonotonicNs() < generated + 250000000ULL, "fixture must enter query before expiry");
            if (rpm) arduino.delay_rpm_ms = 75;
            else arduino.delay_encoder_ms = 75;
            reference = generated + options.command_timeout_ns;
            label = rpm ? "rpm_expiry" : "encoder_expiry";
        } else if (fault) {
            if (rpm) arduino.no_rpm_reply = true;
            else arduino.no_encoder_reply = true;
            label = rpm ? "rpm_timeout" : "encoder_timeout";
        } else if (scenario == Scenario::ExpiredAtEntry) {
            SleepUntil(generated + options.command_timeout_ns);
            reference = generated + options.command_timeout_ns;
            label = "expired_at_entry";
        } else {
            reference = MonotonicNs();
            Publish(input, {2, reference, 0, 0, 0});
            if (scenario == Scenario::DelayedStopAck) arduino.delay_motor_ms = 75;
            label = scenario == Scenario::NormalStop ? "normal_stop" : "delayed_stop_ack";
        }
        const auto first = arduino.Events().size();
        const auto loop_start = MonotonicNs();
        int result;
        std::uint64_t diagnostic_ns;
        {
            DiagnosticProbe probe;
            result = motor.Loop();
            diagnostic_ns = probe.first_ns;
        }
        const auto loop_end = MonotonicNs();
        const auto events = arduino.Events();
        Check(result == (fault ? -ETIMEDOUT : 0), "timing loop result");
        const bool queried = sensor_expiry || fault;
        Check(events.size() == first + (queried ? 2 : 1), "bounded transaction count");
        if (queried)
            Check(events[first].command == (rpm ? 'z' : 'e'), "only selected sensor before stop");
        const auto& stop = events.back();
        Check(stop.command == 'm' && stop.zero, "stop before any further sensor query");
        const auto state = ReadWheels(reader);
        if (fault) {
            Check(diagnostic_ns > stop.timestamp_ns && state.timestamp_ns > stop.timestamp_ns,
                  "stop reached UART before diagnostics and fault Publish");
            Check(!state.command_acknowledged && state.last_error == -ETIMEDOUT,
                  "fault stop ACK cannot clear uncertainty");
            reference = events[first].timestamp_ns; // query arrival -> stop arrival
        } else Check(state.command_acknowledged, "normal zero acknowledged");
        Check(stop.timestamp_ns >= reference, "timing reference precedes stop");
        std::cout << std::fixed << std::setprecision(3)
                  << "TIMING " << label
                  << " reference_to_stop_ms=" << (stop.timestamp_ns - reference) / 1.0e6
                  << " loop_to_stop_ms=" << (stop.timestamp_ns - loop_start) / 1.0e6
                  << " loop_ms=" << (loop_end - loop_start) / 1.0e6
                  << " uart_transactions=" << events.size() - first << '\n';
        Check(guard.Close() != fault, "timing shutdown preserves fault state");
    }
}

void LatestAfterSensor() {
    Arduino arduino;
    const auto prefix = Prefix();
    CommandPublisher input;
    input.Create(prefix);
    auto options = Options(arduino, prefix);
    options.command_timeout_ns = 250000000ULL;
    MotorElement motor(options);
    ElementGuard guard{motor};
    Check(motor.Setup() == 0, "latest-after-sensor setup");
    Publish(input, {1, MonotonicNs(), 0.1, 0, 0});
    Check(motor.Loop() == 0, "initial motion slot");
    const auto initial_target = arduino.LastTarget();
    auto first = arduino.Events().size();
    Publish(input, {2, MonotonicNs(), 0.2, 0, 0});
    Publish(input, {3, MonotonicNs(), 0.3, 0, 0});
    Check(motor.Loop() == 0, "sensor progresses despite new commands");
    auto events = arduino.Events();
    Check(events.size() == first + 1 && events.back().command == 'e' &&
          arduino.LastTarget() == initial_target, "reserve one sensor slot, no command backlog");
    CmdVel latest{4, MonotonicNs(), 0.4, 0, 0};
    Publish(input, latest);
    Check(motor.Loop() == 0, "latest command after sensor");
    WheelRpm expected{};
    Check(InverseKinematics(latest, expected) &&
          Near(arduino.LastTarget()[0], expected[0], 0.0001), "send latest rather than deferred .2 or .3");
    first = arduino.Events().size();
    Publish(input, {5, MonotonicNs(), 0, 0, 0});
    Check(motor.Loop() == 0, "stop overrides reserved RPM slot");
    events = arduino.Events();
    Check(events.size() == first + 1 && events.back().command == 'm' && events.back().zero,
          "explicit stop always wins over sensor fairness");
    Check(guard.Close(), "latest-after-sensor shutdown");
}

struct CommandFeed {
    CommandPublisher& input;
    unsigned hz;
    std::atomic<bool> running{true};
    std::atomic<unsigned> published{0};
    std::atomic<int> error{0};
    std::thread thread;
    CommandFeed(CommandPublisher& owner, unsigned frequency) : input(owner), hz(frequency),
        thread([this] {
            auto tick = std::chrono::steady_clock::now();
            std::uint64_t sequence = 0;
            while (running) {
                CmdVel value{++sequence, MonotonicNs(), 0.05 + sequence * 0.0001, 0, 0};
                const int result = input.pub.Publish(value);
                if (result && result != -EAGAIN) { error = result; break; }
                if (!result) ++published;
                tick += std::chrono::microseconds(1000000 / hz);
                std::this_thread::sleep_until(tick);
            }
        }) {}
    void Stop() { running = false; if (thread.joinable()) thread.join(); }
    ~CommandFeed() { Stop(); }
};
void PrintIntervals(const char* name, const std::vector<std::uint64_t>& times,
                    std::uint64_t start, std::uint64_t end) {
    double sum = 0, maximum = 0;
    for (std::size_t i = 1; i < times.size(); ++i) {
        const double gap = (times[i] - times[i-1]) / 1.0e6;
        sum += gap;
        maximum = std::max(maximum, gap);
    }
    const auto left = times.empty() ? end : times.front();
    const auto right = times.empty() ? start : times.back();
    maximum = std::max(maximum, (left - start) / 1.0e6);
    maximum = std::max(maximum, (end - right) / 1.0e6);
    std::cout << ' ' << name << "_n=" << times.size()
              << " " << name << "_mean_ms=" << (times.size() > 1 ? sum / (times.size()-1) : -1)
              << " " << name << "_max_gap_ms=" << maximum;
}
void ContinuousCommands(bool baseline) {
    for (unsigned delay : {0u, 20u}) for (unsigned hz : {10u, 50u, 100u}) {
        Arduino arduino;
        const auto prefix = Prefix();
        CommandPublisher input;
        input.Create(prefix);
        auto options = Options(arduino, prefix);
        options.command_timeout_ns = 250000000ULL;
        MotorElement motor(options);
        ElementGuard guard{motor};
        Check(motor.Setup() == 0, "continuous setup");
        kcf::Subscriber<WheelState> wheels;
        kcf::Subscriber<WheelOdometry> odom;
        Check(wheels.Create(prefix + "/wheel_state") == 0 &&
              odom.Create(prefix + "/wheel_odometry") == 0, "continuous readers");
        arduino.continuous_delay_ms = delay;
        const auto first = arduino.Events().size();
        const auto start = MonotonicNs();
        CommandFeed feed(input, hz);
        std::vector<std::uint64_t> encoder_times, rpm_times;
        auto previous = ReadWheels(wheels);
        WheelOdometry previous_odom{};
        kcf::TopicReadInfo info{};
        Check(odom.ReadLatest(previous_odom, info) == 0, "initial continuous odometry");
        auto tick = std::chrono::steady_clock::now();
        while (MonotonicNs() - start < 1500000000ULL) {
            const auto before_events = arduino.Events().size();
            Check(motor.Loop() == 0, "continuous loop");
            const auto events = arduino.Events();
            bool encoder_read = false, rpm_read = false;
            for (auto i = before_events; i < events.size(); ++i) {
                encoder_read |= events[i].command == 'e';
                rpm_read |= events[i].command == 'z';
            }
            const auto state = ReadWheels(wheels);
            WheelOdometry pose{};
            Check(odom.ReadLatest(pose, info) == 0, "continuous odometry");
            if (encoder_read) encoder_times.push_back(state.encoder_timestamp_ns);
            else Check(state.encoder_timestamp_ns == previous.encoder_timestamp_ns &&
                       pose.timestamp_ns == previous_odom.timestamp_ns &&
                       pose.sequence == previous_odom.sequence, "no fabricated encoder/odometry sample");
            if (rpm_read) rpm_times.push_back(state.rpm_timestamp_ns);
            else Check(state.rpm_timestamp_ns == previous.rpm_timestamp_ns, "no fabricated RPM sample");
            previous = state;
            previous_odom = pose;
            tick += 15ms;
            if (std::chrono::steady_clock::now() < tick) std::this_thread::sleep_until(tick);
            else tick = std::chrono::steady_clock::now();
        }
        const auto end = MonotonicNs();
        feed.Stop();
        Check(feed.error == 0, "continuous publisher");
        auto events = arduino.Events();
        std::vector<std::uint64_t> commands;
        float last_rpm = 0;
        for (auto i = first; i < events.size(); ++i) if (events[i].command == 'm' && !events[i].zero) {
            commands.push_back(events[i].timestamp_ns);
            Check(events[i].first_rpm <= last_rpm, "command values never replay backwards");
            last_rpm = events[i].first_rpm;
        }
        const auto stop_generated = MonotonicNs();
        Publish(input, {999999, stop_generated, 0, 0, 0});
        const auto before_stop = arduino.Events().size();
        Check(motor.Loop() == 0, "stop during continuous load");
        events = arduino.Events();
        Check(events.size() == before_stop + 1 && events.back().command == 'm' && events.back().zero,
              "stop wins over reserved sensor slot");
        std::cout << std::fixed << std::setprecision(2) << "LOAD " << (baseline ? "before" : "after")
                  << " hz=" << hz << " response_delay_ms=" << delay << " input_n=" << feed.published;
        PrintIntervals("cmd", commands, start, end);
        PrintIntervals("enc", encoder_times, start, end);
        PrintIntervals("rpm", rpm_times, start, end);
        std::cout << " stop_ms=" << (events.back().timestamp_ns - stop_generated) / 1.0e6 << '\n';
        if (!baseline) {
            Check(encoder_times.size() >= 3 && rpm_times.size() >= 3, "both sensors progress under continuous commands");
            // Bounded count of consecutive motion transmissions, not a wall-clock guarantee.
            unsigned consecutive = 0;
            for (auto i = first; i < before_stop; ++i) {
                if (events[i].command == 'm' && !events[i].zero)
                    Check(++consecutive <= 1, "motion cannot indefinitely displace sensor slot");
                if (events[i].command == 'e' || events[i].command == 'z') consecutive = 0;
            }
        }
        Check(guard.Close(), "continuous shutdown");
    }
}
void LatestDuringAck() {
    enum class Arrival { Same, Changed, Zero, Invalid, None };
    for (const auto arrival : {Arrival::Same, Arrival::Changed, Arrival::Zero, Arrival::Invalid, Arrival::None}) {
        Arduino arduino;
        const auto prefix=Prefix();
        CommandPublisher input; input.Create(prefix);
        auto options=Options(arduino,prefix); options.command_timeout_ns=250000000ULL;
        MotorElement motor(options); ElementGuard guard{motor};
        Check(motor.Setup()==0,"ACK reselection setup");
        const auto generated=MonotonicNs();
        Publish(input,{1,generated,0.1,0,0});
        SleepUntil(generated+190000000ULL);
        arduino.delay_motor_ms=80;
        const auto first=arduino.Events().size();
        std::atomic<int> error{0};
        std::thread producer([&] {
            std::this_thread::sleep_for(20ms);
            if (arrival==Arrival::None) return;
            const double vx=arrival==Arrival::Zero ? 0 : arrival==Arrival::Changed ? 0.2 :
                arrival==Arrival::Invalid ? std::numeric_limits<double>::quiet_NaN() : 0.1;
            error=input.pub.Publish({2,MonotonicNs(),vx,0,0});
        });
        const int result=motor.Loop();
        producer.join();
        Check(result==0 && error==0,"ACK reselection Loop");
        const auto events=arduino.Events();
        const bool valid=arrival==Arrival::Same || arrival==Arrival::Changed;
        Check(events.size()==first+(valid ? 1 : 2),"only required stop may follow ACK");
        Check(events.back().command=='m' && events.back().zero==!valid,
              "latest input, not expired pre-ACK selection, decides stop");
        if (valid) {
            Check(motor.Loop()==0,"sensor slot retained after ACK renewal");
            Check(arduino.Events().back().command=='e',"renewal does not starve encoder");
            Check(motor.Loop()==0,"latest motion is then transmitted");
            WheelRpm expected{};
            Check(InverseKinematics({2,MonotonicNs(),arrival==Arrival::Changed ? 0.2 : 0.1,0,0},expected),"expected renewal");
            Check(Near(arduino.LastTarget()[0],expected[0],0.0001),"latest target after reserved sensor");
        }
        Check(guard.Close(),"ACK reselection shutdown");
    }
}
void ExactIntervals(const char* name, const std::vector<std::uint64_t>& times) {
    double sum=0, maximum=0;
    for (std::size_t i=1; i<times.size(); ++i) {
        const double gap=(times[i]-times[i-1])/1e6;
        sum+=gap; maximum=std::max(maximum,gap);
    }
    std::cout << ' ' << name << "_n=" << times.size()
              << ' ' << name << "_mean_ms=" << (times.size()>1 ? sum/(times.size()-1) : -1)
              << ' ' << name << "_max_ms=" << maximum;
}
void WatchdogReceiveIntervals(bool baseline = false) {
    // Constant body velocity => constant wheel RPM. Only generation time/sequence
    // change. PTY receiver timestamps complete validated frames, BEFORE ACK delay.
    for (unsigned delay : {0u,20u,80u}) for (unsigned hz : {10u,50u,100u}) {
        Arduino arduino;
        const auto prefix=Prefix();
        CommandPublisher input; input.Create(prefix);
        auto options=Options(arduino,prefix);
        options.command_timeout_ns=250000000ULL;
        MotorElement motor(options); ElementGuard guard{motor};
        Check(motor.Setup()==0,"watchdog measurement setup");
        std::vector<MotorCommandObservation> observations;
        MotorElementTestAccess::Observe(motor,observations);
        arduino.strict_motor_validation=true; // synthetic receiver limit, not a robot setting
        arduino.continuous_delay_ms=delay;
        const auto first=arduino.Events().size();
        uart_trace::Capture tx;
        std::vector<std::uint64_t> generation;
        CmdVel last{};
        std::atomic<bool> running{true};
        std::atomic<int> publish_error{0};
        std::atomic<unsigned> skipped{0};
        const auto start=MonotonicNs();
        std::thread producer([&] {
            auto tick=std::chrono::steady_clock::now();
            std::uint64_t seq=0;
            while (running) {
                CmdVel value{++seq,MonotonicNs(),0.05,0,0};
                generation.push_back(value.generated_at_ns);
                const int result=input.pub.Publish(value);
                if (!result) last=value;
                else if (result==-EAGAIN) ++skipped;
                else { publish_error=result; break; }
                tick+=std::chrono::microseconds(1000000/hz);
                std::this_thread::sleep_until(tick);
            }
        });
        int loop_error=0;
        auto tick=std::chrono::steady_clock::now();
        while (MonotonicNs()-start<3000000000ULL) {
            { uart_trace::Scope trace(tx); loop_error=motor.Loop(); }
            if (loop_error) break;
            tick+=15ms;
            if (std::chrono::steady_clock::now()<tick) std::this_thread::sleep_until(tick);
            else tick=std::chrono::steady_clock::now();
        }
        const auto end=MonotonicNs();
        running=false; producer.join();
        Check(loop_error==0 && publish_error==0 && last.generated_at_ns!=0,"watchdog live feed");
        // Stop publication through expiry + 150 ms, then replay the same expired
        // sample. Check real producer loss first, and no revival by stale receipts.
        const auto expires=last.generated_at_ns+options.command_timeout_ns;
        while (MonotonicNs()<expires+350000000ULL) {
            if (MonotonicNs()>=expires+150000000ULL) {
                const int result=input.pub.Publish(last);
                Check(result==0 || result==-EAGAIN,"stale replay publish");
            }
            { uart_trace::Scope trace(tx); Check(motor.Loop()==0,"stale replay Loop"); }
            std::this_thread::sleep_for(15ms);
        }
        const auto events=arduino.Events();
        std::vector<std::uint64_t> gen, sent, received, motion, encoder, rpm;
        for (auto t:generation) if (t<end) gen.push_back(t);
        std::vector<Arduino::Event> all_received;
        unsigned live_zeros=0;
        std::uint64_t expired_stop=0;
        for (auto i=first;i<events.size();++i) {
            const auto& e=events[i];
            if (e.command=='m') {
                all_received.push_back(e);
                if (e.timestamp_ns<end) {
                    received.push_back(e.timestamp_ns);
                    if (!e.zero) motion.push_back(e.timestamp_ns); else ++live_zeros;
                }
                if (e.zero && e.timestamp_ns>=expires && !expired_stop) expired_stop=e.timestamp_ns;
            }
            if (e.timestamp_ns<end && e.command=='e') encoder.push_back(e.timestamp_ns);
            if (e.timestamp_ns<end && e.command=='z') rpm.push_back(e.timestamp_ns);
        }
        Check(all_received.size()==tx.motors.size(),"one received frame per traced host frame");
        double transport_max=0;
        double send_age_max=0;
        double live_send_age_max=0;
        std::vector<MotorCommandObservation> selections;
        for (const auto& o:observations)
            if (std::string(o.phase)=="send" || std::string(o.phase)=="ack_expired") selections.push_back(o);
        Check(selections.size()==tx.motors.size(),"each write has an observed selection");
        unsigned stale_motion=0;
        for (std::size_t i=0;i<tx.motors.size();++i) {
            const auto& frame=tx.motors[i];
            const auto& rx=all_received[i];
            Check(rx.timestamp_ns>=frame.start_ns,"receive follows write start");
            std::istringstream values(frame.bytes);
            char m; float first_rpm; values>>m>>first_rpm;
            Check(m=='m' && Near(first_rpm,rx.first_rpm,0.0001),"TX/RX FIFO correspondence");
            if (frame.start_ns<end) {
                sent.push_back(frame.start_ns);
                transport_max=std::max(transport_max,(rx.timestamp_ns-frame.start_ns)/1e6);
            }
            if (!rx.zero) {
                const auto age=frame.start_ns-selections[i].generated_ns;
                Check(age<250000000ULL,"nonzero TX starts with original generation age below 250 ms");
                send_age_max=std::max(send_age_max,age/1e6);
                if (frame.start_ns<end) live_send_age_max=std::max(live_send_age_max,age/1e6);
            }
            if (frame.start_ns>=expires && !rx.zero) ++stale_motion;
        }
        Check(stale_motion==0 && expired_stop!=0,"expired replay sends no nonzero command");
        Check(expired_stop<expires+150000000ULL,"producer loss stops before stale replay begins");
        Check(encoder.size()>3 && rpm.size()>3 && motion.size()>3,"commands and both sensors progress");
        if (!baseline) Check(live_zeros==0,"no unnecessary stops during continuous valid commands");
        std::cout << std::fixed << std::setprecision(3) << "WATCHDOG hz=" << hz << " reply_delay_ms=" << delay;
        ExactIntervals("gen",gen); ExactIntervals("tx",sent); ExactIntervals("rx",received);
        ExactIntervals("motion_rx",motion); ExactIntervals("encoder_rx",encoder); ExactIntervals("rpm_rx",rpm);
        std::cout << " tx_to_rx_max_ms=" << transport_max << " live_zeros=" << live_zeros
                  << " send_age_max_ms=" << send_age_max
                  << " live_send_age_max_ms=" << live_send_age_max
                  << " skipped_publish=" << skipped << " expired_nonzero=" << stale_motion
                  << " expiry_to_zero_rx_ms=" << (expired_stop-expires)/1e6 << '\n';
        // Full timestamps make generation/write/validation observable separately.
        for (auto t:gen) std::cout << "STAMP " << hz << ' ' << delay << " gen " << t << '\n';
        for (const auto& o:observations)
            std::cout << "DECISION " << hz << ' ' << delay << ' ' << o.phase << ' '
                      << o.selected_ns << ' ' << o.checked_ns << ' ' << o.generated_ns << ' '
                      << o.received_ns << ' ' << o.latest_generated_ns << ' ' << o.latest_received_ns << ' '
                      << o.applied_expires_ns << ' ' << o.fresh << ' ' << o.stop_only << ' ' << o.zero << '\n';
        for (std::size_t i=0;i<tx.motors.size();++i)
            std::cout << "STAMP " << hz << ' ' << delay << " m " << tx.motors[i].start_ns << ' '
                      << tx.motors[i].complete_ns << ' ' << all_received[i].timestamp_ns << ' '
                      << all_received[i].zero << '\n';
        Check(guard.Close(),"watchdog measurement shutdown");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc==2 && (std::string(argv[1])=="--watchdog-intervals" ||
                       std::string(argv[1])=="--watchdog-expiry-baseline")) {
            WatchdogReceiveIntervals(std::string(argv[1])=="--watchdog-expiry-baseline");
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--continuous-baseline") {
            ContinuousCommands(true);
            return 0;
        }
        MathAndContracts();
        StartupFrames();
        DriverProtocol();
        CommandErrorClassification();
        ElementLifecycle();
        FailureCases();
        DistributedTelemetry();
        StopOrderAndLatency();
        LatestAfterSensor();
        LatestDuringAck();
        ContinuousCommands(false);
        std::cout << "PASS: kinematics, contracts, PTY protocol, Topic lifecycle, freshness and stop failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
