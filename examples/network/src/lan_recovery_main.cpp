// Keep the Local Parameter owner alive while restarting only its Network gateway.
#include "kcf/network/control_node.hpp"
#include "kcf/parameter/parameter.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace n = kcf::network;
namespace {
volatile std::sig_atomic_t stopped = 0;
void Signal(int) { stopped = 1; }

bool Id(const std::string& text, n::HostIdentity& id) {
    std::string hex;
    for (char c : text) if (c != '-') hex += c;
    if (hex.size() != 32) return false;
    auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (unsigned i = 0; i < 16; ++i) {
        int a = digit(hex[2*i]), b = digit(hex[2*i+1]);
        if (a < 0 || b < 0) return false;
        id.bytes[i] = static_cast<std::uint8_t>((a << 4) | b);
    }
    return n::Valid(id);
}

std::uint16_t Port(const char* text) {
    const std::string s(text);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected decimal port");
    auto port = std::stoul(s);
    if (!port || port > 65535) throw std::invalid_argument("port out of range");
    return static_cast<std::uint16_t>(port);
}

void Name(const std::string& name) {
    if (!n::Text(name, 240) || name.front() != '/')
        throw std::invalid_argument("expected /parameter/name (at most 240 bytes)");
}

void Usage() {
    std::cout << "Usage:\n"
        "  kcf_lan_recovery owner NAME\n"
        "  kcf_lan_recovery gateway HOST_HEX LOCAL_IPV4 DISCOVERY_PORT CONTROL_PORT TOOL_HOST_HEX NAME\n"
        "Start owner and gateway as separate processes. Stop/restart only gateway.\n"
        "Tool gets Descriptor Query and Parameter Get only; no Set permission.\n";
}

struct OwnerResources {
    kcf::Parameter<Value> parameter;
    bool created{false};
    OwnerResources() {
        kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE);
    }
    ~OwnerResources() {
        const int closed = parameter.Close();
        const int unlinked = created ? parameter.Unlink() : 0;
        kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED, 0);
        std::cout << "Owner stopped pid=" << getpid()
                  << " close=" << closed << " unlink=" << unlinked << '\n';
    }
};

int Owner(const std::string& name) {
    OwnerResources resources;
    const int code = resources.parameter.Create(name, {7, true, 1.5});
    if (code) {
        std::cerr << "Owner Create error=" << code << '\n';
        return 1;
    }
    resources.created = true;
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING, 0);
    std::cout << "Owner ready pid=" << getpid() << " parameter=" << name << '\n';
    while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

int Gateway(const n::ControlConfig& config, const std::string& host) {
    n::ControlNode node;
    std::cout << "Gateway starting pid=" << getpid() << " host=" << host
              << " control_port=" << config.discovery.metadata_port << '\n';
    int code = node.Start(config);
    if (code) std::cerr << "Gateway Start error=" << code << '\n';
    else {
        std::cout << "Gateway ready pid=" << getpid() << " parameter="
                  << config.metadata.remote_parameters.front() << '\n';
        while (!stopped) {
            code = node.GetLastError();
            if (code) {
                std::cerr << "Gateway worker error=" << code << '\n';
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    node.Stop();
    std::cout << "Gateway stopped pid=" << getpid() << " error=" << code << '\n';
    return code ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc == 2 && std::string(argv[1]) == "--help") { Usage(); return 0; }
    bool owner = false;
    std::string name;
    n::ControlConfig config;
    try {
        if (argc < 2) throw std::invalid_argument("missing mode");
        const std::string mode = argv[1];
        owner = mode == "owner";
        if ((owner && argc != 3) || (!owner && (mode != "gateway" || argc != 8)))
            throw std::invalid_argument("invalid mode/argument count");
        name = argv[owner ? 2 : 7];
        Name(name);
        if (!owner) {
            n::HostIdentity tool;
            if (!Id(argv[2], config.discovery.host) || !Id(argv[6], tool) ||
                tool == config.discovery.host)
                throw std::invalid_argument("invalid or duplicate Host ID");
            in_addr ip{};
            if (inet_pton(AF_INET, argv[3], &ip) != 1 || ntohl(ip.s_addr) == 0 ||
                ntohl(ip.s_addr) >= 0xe0000000u)
                throw std::invalid_argument("expected local unicast IPv4");
            config.discovery.interface_address = argv[3];
            config.discovery.port = Port(argv[4]);
            config.discovery.metadata_port = Port(argv[5]);
            if (config.discovery.port == config.discovery.metadata_port)
                throw std::invalid_argument("discovery/control ports must differ");
            config.metadata.remote_parameters = {name};
            // Empty runtime_pids includes the independent Local Owner process.
            config.metadata.runtime_pids.clear();
            config.descriptor_query_clients = {tool};
            config.parameter_get_clients = {tool};
            config.topics.enabled = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Arguments: " << e.what() << '\n'; Usage(); return 2;
    }
    std::signal(SIGINT, Signal);
    std::signal(SIGTERM, Signal);
    try {
        if (owner) return Owner(name);
        std::string boot;
        std::ifstream("/proc/sys/kernel/random/boot_id") >> boot;
        if (!Id(boot, config.discovery.boot_id)) {
            std::cerr << "Gateway invalid Linux boot identity\n"; return 1;
        }
        return Gateway(config, argv[2]);
    } catch (const std::exception& e) {
        std::cerr << "Runtime error: " << e.what() << '\n'; return 1;
    }
}
