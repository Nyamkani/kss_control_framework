#include "kcf/network/control_node.hpp"
#include "kcf/network/validation.hpp"
#include "kcf/process/execution_mode.hpp"
#include "kcf/process/process_runtime.hpp"
#include <arpa/inet.h>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {
namespace network = kcf::network;

void Usage() {
    std::cout << "Usage: kcf_mecanum_gateway --host-id ID --local-ip IPV4\n"
                 "       [--discovery-port 37651] [--control-port 39001] [--tools-host-id 32HEX]\n"
                 "ID: nonzero 32 hexadecimal digits or canonical UUID; keep stable per host.\n"
                 "--control-port: shared numeric UDP metadata / TCP control port.\n"
                 "boot_id: /proc/sys/kernel/random/boot_id\n"
                 "Standalone discovery gateway; all local runtime metadata is collected.\n"
                 "Topics/Diagnostics: disabled by default; --tools-host-id enables both.\n"
                 "Topic UDP port 37652, default rate limit 50 Hz; exposed Topics (5):\n"
                 "  /mecanum/wheel_state, /mecanum/wheel_odometry, /mecanum/imu/raw_data,\n"
                 "  /mecanum/imu/mag_raw, /mecanum/ekf/odometry\n"
                 "Only the specified Tools Host ID may query Type Descriptors.\n"
                 "Tools Host ID must differ from Gateway Host ID. Remote control grants remain disabled.\n";
}

bool ParseId(const std::string& text, network::HostIdentity& id) {
    if (text.size() != 32 && text.size() != 36) return false;
    std::string hex;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (text[i] != '-') return false;
        } else hex += text[i];
    }
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        unsigned byte = 0;
        const char* begin = hex.data() + 2 * i;
        const auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2) return false;
        id.bytes[i] = static_cast<std::uint8_t>(byte);
    }
    return network::Valid(id);
}

bool ParsePort(const std::string& text, std::uint16_t& port) {
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0 || value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

class GatewayElement final : public kcf::ProcessElement {
public:
    explicit GatewayElement(network::ControlConfig config) : config_(std::move(config)) {}
    int Setup() override {
        const int result = node_.Start(config_);
        if (result) std::cerr << "Gateway start failed: " << result << '\n';
        else std::cout << "Gateway started: local_ip=" << config_.discovery.interface_address
                       << " discovery_port=" << config_.discovery.port
                       << " metadata/control_port=" << config_.discovery.metadata_port
                       << " topics=" << (config_.topics.enabled ? "enabled" : "disabled")
                       << " diagnostics=" << (config_.diagnostics_enabled ? "enabled" : "disabled")
                       << " exposed_topics=" << config_.metadata.remote_topics.size()
                       << " topic_udp_port=" << config_.topics.port
                       << (config_.topics.enabled ? "" : " (inactive)") << '\n';
        return result;
    }
    int Loop() override {
        const int result = node_.GetLastError();
        if (result) std::cerr << "Gateway network error: " << result << '\n';
        return result;
    }
    void Shutdown() override {
        // Also cleans up a partially failed Start; owns no robot processes or Topics.
        node_.Stop();
        std::cout << "Gateway stopped\n";
    }
private:
    network::ControlConfig config_;
    network::ControlNode node_;
};
} // namespace

int main(int argc, char** argv) {
    network::ControlConfig config;
    config.discovery.port = 37651;
    config.discovery.metadata_port = 39001;
    config.topics.enabled = false;
    // Remote exposure and client grants stay empty unless explicitly enabled below. Empty runtime_pids collects all local runtimes, preserving
    // the Framework's existing Supervisor Application/Element associations.
    bool have_host = false, have_address = false, have_tools_host = false;
    network::HostIdentity tools_host;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { Usage(); return 0; }
        if (++i >= argc) { Usage(); return 2; }
        const std::string value = argv[i];
        bool valid = false;
        if (key == "--host-id") valid = have_host = ParseId(value, config.discovery.host);
        else if (key == "--tools-host-id")
            valid = have_tools_host = value.size() == 32 && ParseId(value, tools_host);
        else if (key == "--local-ip") {
            in_addr address{};
            valid = have_address = inet_pton(AF_INET, value.c_str(), &address) == 1 &&
                ntohl(address.s_addr) != INADDR_ANY && ntohl(address.s_addr) < 0xe0000000U;
            if (valid) config.discovery.interface_address = value;
        } else if (key == "--discovery-port") valid = ParsePort(value, config.discovery.port);
        else if (key == "--control-port") valid = ParsePort(value, config.discovery.metadata_port);
        if (!valid) { std::cerr << "Invalid argument: " << key << '\n'; return 2; }
    }
    if (!have_host || !have_address || config.discovery.port == config.discovery.metadata_port) {
        Usage();
        return 2;
    }
    if (have_tools_host) {
        if (tools_host == config.discovery.host) {
            std::cerr << "Tools Host ID must differ from Gateway Host ID\n";
            return 2;
        }
        if (config.topics.port == config.discovery.port ||
            config.topics.port == config.discovery.metadata_port) {
            std::cerr << "Topic UDP port conflicts with Discovery/Control port\n";
            return 2;
        }
        config.topics.enabled = true;
        config.diagnostics_enabled = true;
        config.metadata.remote_topics = {
            "/mecanum/wheel_state", "/mecanum/wheel_odometry",
            "/mecanum/imu/raw_data", "/mecanum/imu/mag_raw", "/mecanum/ekf/odometry"};
        config.descriptor_query_clients = {tools_host};
        // No cmd_vel exposure or Parameter/Service/Action permissions are granted.
    }
    if (kcf::DetectLaunchExecutionMode() != kcf::ExecutionMode::STANDALONE) {
        std::cerr << "Gateway must be launched as a standalone process\n";
        return 2;
    }
    std::string boot_id;
    std::ifstream boot_file("/proc/sys/kernel/random/boot_id");
    if (!(boot_file >> boot_id) || !ParseId(boot_id, config.discovery.boot_id)) {
        std::cerr << "Cannot read a valid Linux boot_id\n";
        return 1;
    }
    GatewayElement element(std::move(config));
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(10.0);
    const int result = runtime.Run(element);
    if (result) std::cerr << "Gateway runtime failed: " << result << '\n';
    return result ? 1 : 0;
}
