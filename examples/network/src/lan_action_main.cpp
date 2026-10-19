// Physical LAN example: public Network API and the existing descriptor/field codec.
#include "kcf/network/control_node.hpp"
#include "kcf/action/action_server.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "parameter_demo_type.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace n = kcf::network;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace {
constexpr const char* Name = "/network/test_action";
volatile std::sig_atomic_t stopped = 0;
void Signal(int) { stopped = 1; }

void Check(int error, const char* stage) {
    if (!error) return;
    const char* category = error == -ESTALE ? "session/identity mismatch" :
        error == -ETIMEDOUT ? "timeout (remote execution may be unknown)" :
        error == -ECONNRESET || error == -ECONNREFUSED || error == -ENOTCONN ||
        error == -EHOSTUNREACH || error == -ENETUNREACH ? "connection failure" :
        error == -EINTR ? "interrupted" : "API/protocol error";
    throw std::runtime_error(std::string(stage) + ": " + category +
        " error=" + std::to_string(error) + " (" + std::strerror(-error) + ")");
}
bool Id(const std::string& text, n::HostIdentity& id) {
    std::string hex;
    for (char c : text) if (c != '-') hex += c;
    if (hex.size() != 32) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c-'0' :
        c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
    for (unsigned i=0; i<16; ++i) {
        int a=digit(hex[2*i]), b=digit(hex[2*i+1]);
        if (a<0 || b<0) return false;
        id.bytes[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    return n::Valid(id);
}
int Number(const char* text, int maximum) {
    std::string s(text);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected positive decimal number");
    const auto value = std::stoul(s);
    if (!value || value > static_cast<unsigned>(maximum)) throw std::invalid_argument("number out of range");
    return static_cast<int>(value);
}
void Usage() {
    std::cout << "Usage:\n"
        "  kcf_lan_action owner HOST_HEX LOCAL_IPV4 DISCOVERY_PORT CONTROL_PORT\n"
        "  kcf_lan_action client HOST_HEX LOCAL_IPV4 DISCOVERY_PORT CONTROL_PORT REMOTE_HOST_HEX goal|cancel [COUNT]\n"
        "HOST_HEX: nonzero 128-bit hex ID (optional UUID hyphens); unique per host.\n"
        "COUNT: 1..1000000, default 7; success result is COUNT*2, cancel result -1.\n"
        "Both hosts use the same discovery port and multicast 239.255.75.67.\n"
        "Allow discovery UDP and each CONTROL_PORT TCP + UDP on the LAN interface.\n"
        "Timeouts: discovery 10s, goal/cancel ack 3s (+1s wait guard), feedback/result 8s.\n"
        "Owner runs until SIGINT/SIGTERM; requests are never automatically retried.\n";
}
auto Descriptor() { return kcf::TypeDescriptorTraits<Value>::Get(); }
n::TypeIdentity Type() {
    n::TypeIdentity type;
    Check(n::ParameterWireType(Descriptor(), type), "type");
    return type;
}
std::vector<std::uint8_t> Encode(int count) {
    Value value{count, false, 1.5};
    kcf::DynamicPayload payload;
    payload.type_id=Type().type_id; payload.bytes.resize(sizeof(value));
    std::memcpy(payload.bytes.data(), &value, sizeof(value));
    std::vector<std::uint8_t> wire;
    Check(n::EncodeParameterValue(Descriptor(), payload, wire), "encode");
    return wire;
}
Value Decode(const std::vector<std::uint8_t>& wire) {
    kcf::DynamicPayload payload;
    Check(n::DecodeParameterValue(Descriptor(), wire, payload), "decode");
    if (payload.bytes.size()!=sizeof(Value)) Check(-EPROTO, "decoded size");
    Value value{}; std::memcpy(&value, payload.bytes.data(), sizeof(value));
    return value;
}
template<class Predicate>
void Until(Predicate ready, std::chrono::milliseconds timeout, const char* stage) {
    const auto deadline=Clock::now()+timeout;
    for (;;) {
        if (stopped) Check(-EINTR, stage);
        if (ready()) return;
        if (Clock::now()>=deadline) Check(-ETIMEDOUT, stage);
        std::this_thread::sleep_for(10ms);
    }
}
n::ParameterEndpointResult Result(std::future<n::ActionBatchResult> future, const char* stage) {
    Until([&] { return future.wait_for(0ms)==std::future_status::ready; }, 4s, stage);
    auto batch=future.get(); Check(batch.error, stage);
    if (batch.endpoints.size()!=1) Check(-EPROTO, "expected exactly one Action endpoint");
    auto result=batch.endpoints.front();
    std::cout << stage << " error=" << result.error
        << " reason=" << n::CompatibilityReasonName(result.reason)
        << " execution_unknown=" << result.execution_unknown
        << " cancel_unknown=" << result.cancel_unknown << '\n';
    Check(result.error, stage);
    return result;
}
std::uint16_t LocalServicePort() {
    int fd=socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0);
    if (fd<0) { Check(-errno, "local socket"); }
    sockaddr_in address{}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int error=0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) error=-errno;
    socklen_t size=sizeof(address);
    if (!error && getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size)) error=-errno;
    close(fd); Check(error, "local port allocation");
    return ntohs(address.sin_port); // Create reports a race/collision; no silent retry.
}
struct OwnerResources {
    kcf::ServiceServer service;
    kcf::ActionServer<Value, Value, Value> action;
    n::ControlNode node;
    bool created=false;
    OwnerResources() { kcf::detail::BeginRuntimeIntrospection(kcf::ExecutionMode::STANDALONE); }
    ~OwnerResources() {
        node.Stop(); service.Stop();
        if (created) {
            const int code=action.Unlink();
            if (code) std::cerr << "Action Unlink error=" << code << '\n';
        }
        action.Close();
        kcf::detail::EndRuntimeIntrospection(kcf::ProcessState::STOPPED, 0);
        std::cout << "Owner shutdown complete\n";
    }
};
void Owner(n::ControlConfig config) {
    // Callback capture outlives service Stop and destruction.
    std::atomic<int> goal_value{0};
    OwnerResources resources;
    Check(resources.service.Create(LocalServicePort()), "Service Create");
    Check(resources.action.Create(resources.service, {1,2,3}, Name,
        [&](std::uint64_t, const Value& value) {
            if (value.count<=0 || value.count>1000000 || value.enabled || value.gain!=1.5) return -EINVAL;
            goal_value=value.count; return 0;
        }), "Action Create");
    resources.created=true;
    Check(resources.service.Start(), "Service Start");
    config.metadata.remote_actions={Name};
    kcf::detail::UpdateRuntimeIntrospection(kcf::ProcessState::RUNNING, 0);
    Check(resources.node.Start(config), "Control Start");
    std::cout << "Owner ready " << Name << " REMOTE control_port=" << config.discovery.metadata_port << '\n';
    auto start=Clock::now(), next=start;
    int ticks=0;
    while (!stopped) {
        Check(resources.node.GetLastError(), "Control worker");
        auto& action=resources.action;
        auto id=action.GetGoalId(); auto state=action.GetState();
        if ((state==kcf::ActionState::ACCEPTED || state==kcf::ActionState::RUNNING) && action.IsCancelRequested(id)) {
            Check(action.Canceled(id, {-1,false,1.5}), "Canceled");
            std::cout << "Owner CANCELED goal=" << id << '\n';
        } else if (state==kcf::ActionState::ACCEPTED) {
            int code=action.Start(id);
            if (code!=-EINVAL || !action.IsCancelRequested(id)) Check(code, "Action Start");
            start=next=Clock::now(); ticks=0;
        } else if (state==kcf::ActionState::RUNNING) {
            if (Clock::now()-start>=2s) {
                Check(action.Succeed(id, {goal_value.load()*2,false,1.5}), "Succeed");
                std::cout << "Owner SUCCEEDED goal=" << id << '\n';
            } else if (Clock::now()>=next) {
                int code=action.PublishFeedback(id, {++ticks,false,1.5});
                if (code!=-EAGAIN) Check(code, "Feedback");
                next=Clock::now()+100ms;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
}
void Client(const n::ControlConfig& config, n::HostIdentity remote, bool cancel, int count) {
    n::ControlNode node;
    Check(node.Start(config), "Control Start");
    n::PeerInfo selected;
    Until([&] {
        Check(node.GetLastError(), "Control worker");
        for (const auto& peer : node.GetPeers()) {
            if (peer.host!=remote || peer.state!=n::PeerState::ONLINE) continue;
            if (!peer.compatibility.Compatible()) {
                std::cerr << "Peer incompatible: " << n::CompatibilityReasonName(peer.compatibility.reason) << '\n';
                Check(-EPROTONOSUPPORT, "discovery");
            }
            if (peer.metadata_ready) for (const auto& endpoint : peer.endpoints)
                if (endpoint.kind==n::EndpointKind::ACTION && endpoint.name==Name && endpoint.scope==n::NetworkScope::REMOTE) {
                    selected=peer; return true;
                }
        }
        return false;
    }, 10s, "discovery/Action metadata");
    std::cout << "Discovered owner address=" << selected.address << " session=" << selected.session_id << '\n';
    auto fresh = [&] {
        for (const auto& peer : node.GetPeers()) if (peer.host==remote) {
            if (peer.boot_id!=selected.boot_id || peer.session_id!=selected.session_id) Check(-ESTALE, "peer");
            if (peer.state!=n::PeerState::ONLINE) Check(-ENOTCONN, "peer lost");
            return;
        }
        Check(-ENOTCONN, "peer absent");
    };
    n::RemoteTarget target; target.scope=n::TargetScope::HOST; target.host=remote;
    fresh();
    auto accepted=Result(node.StartAction(target, Name, Type(), Type(), Type(), Encode(count), 3s), "Goal ack");
    if (!accepted.action || !accepted.action->Snapshot().accepted) Check(-EPROTO, "Goal not accepted");
    auto handle=accepted.action;
    std::cout << "Goal accepted\n";
    std::uint64_t feedback=0;
    auto snapshot = [&] {
        fresh();
        auto s=handle->Snapshot();
        if (s.communication_lost || s.error) {
            std::cerr << "execution_unknown=" << s.execution_unknown << " cancel_unknown=" << s.cancel_unknown << '\n';
            Check(s.error ? s.error : -ECONNRESET, "Action tracking");
        }
        if (s.feedback_sequence>feedback) {
            feedback=s.feedback_sequence;
            auto value=Decode(s.feedback);
            std::cout << "Feedback sequence=" << feedback << " count=" << value.count << '\n';
        }
        return s;
    };
    Until([&] { snapshot(); return feedback!=0; }, 8s, "Feedback");
    if (cancel) {
        Result(node.CancelAction(handle, 3s), "Cancel ack");
        if (!handle->Snapshot().cancel_accepted) Check(-EPROTO, "Cancel not accepted");
        std::cout << "Cancel acknowledged (terminal completion checked separately)\n";
    }
    Until([&] { return snapshot().terminal; }, 8s, "terminal result");
    auto final=snapshot(); auto value=Decode(final.result);
    auto expected=cancel ? kcf::ActionState::CANCELED : kcf::ActionState::SUCCEEDED;
    if (final.state!=expected || value.count!=(cancel ? -1 : count*2) || value.enabled || value.gain!=1.5)
        Check(-EPROTO, "unexpected state/result");
    std::cout << (cancel ? "CANCELED" : "SUCCEEDED") << " result=" << value.count << " PASS\n";
    node.Stop();
}
} // namespace
int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc==2 && std::string(argv[1])=="--help") { Usage(); return 0; }
    n::ControlConfig config;
    n::HostIdentity remote;
    bool owner=false, cancel=false;
    int count=7;
    try {
        if (argc<6) throw std::invalid_argument("missing arguments");
        std::string mode=argv[1]; owner=mode=="owner";
        if ((owner && argc!=6) || (!owner && (mode!="client" || (argc!=8 && argc!=9))))
            throw std::invalid_argument("invalid mode/argument count");
        if (!Id(argv[2], config.discovery.host)) throw std::invalid_argument("invalid Host ID");
        in_addr ip{};
        if (inet_pton(AF_INET,argv[3],&ip)!=1 || ntohl(ip.s_addr)==0 || ntohl(ip.s_addr)>=0xe0000000u)
            throw std::invalid_argument("expected local unicast IPv4");
        config.discovery.interface_address=argv[3];
        config.discovery.port=Number(argv[4],65535);
        config.discovery.metadata_port=Number(argv[5],65535);
        if (config.discovery.port==config.discovery.metadata_port) throw std::invalid_argument("discovery/control ports must differ");
        if (!owner) {
            if (!Id(argv[6],remote) || remote==config.discovery.host) throw std::invalid_argument("invalid/self remote Host ID");
            std::string op=argv[7];
            if (op!="goal" && op!="cancel") throw std::invalid_argument("expected goal or cancel");
            cancel=op=="cancel";
            if (argc==9) count=Number(argv[8],1000000);
        }
    } catch (const std::exception& e) { std::cerr << "Arguments: " << e.what() << '\n'; Usage(); return 2; }
    std::signal(SIGINT,Signal); std::signal(SIGTERM,Signal);
    try {
        std::string boot; std::ifstream("/proc/sys/kernel/random/boot_id") >> boot;
        if (!Id(boot,config.discovery.boot_id)) Check(-EINVAL, "Linux boot identity");
        config.metadata.runtime_pids={getpid()};
        if (owner) Owner(config); else Client(config,remote,cancel,count);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nNo automatic retry/cancel on disconnect; remote execution may continue.\n";
        return stopped ? 130 : 1;
    }
}
