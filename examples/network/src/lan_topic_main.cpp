#include "kcf/network/control_node.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/process/process_runtime.hpp"
#include "parameter_demo_type.hpp"
#include <arpa/inet.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <unistd.h>

namespace n = kcf::network;
using Clock = std::chrono::steady_clock;
namespace {
constexpr const char* Topic = "/network/test_topic";
bool Id(const std::string& text, n::HostIdentity& id) {
    std::string s;
    for (char c : text) if (c!='-') s+=c;
    if (s.size()!=32) return false;
    auto hex=[](char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; };
    for (unsigned i=0;i<16;++i) {
        int a=hex(s[2*i]),b=hex(s[2*i+1]);
        if (a<0||b<0) return false;
        id.bytes[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    return n::Valid(id);
}
int Port(const std::string& s) {
    if (s.empty()||s.find_first_not_of("0123456789")!=std::string::npos) throw std::invalid_argument("invalid port");
    auto value=std::stoul(s);
    if (!value||value>65535) throw std::invalid_argument("port outside 1..65535");
    return static_cast<int>(value);
}
void Usage() {
    std::cout << "Usage: kcf_lan_topic publisher|subscriber HOST_HEX LOCAL_IPV4 DISCOVERY_PORT CONTROL_PORT TOPIC_PORT\n"
        "Both hosts require the same Discovery and Topic UDP ports. Control port uses TCP and UDP.\n"
        "Topic: /network/test_topic; existing Value.count is the application sequence, gain=count*2, enabled=true.\n"
        "Publisher ~200 Hz; gateway <=50 Hz; Diagnostics opt-in ON. Stop with SIGINT/SIGTERM.\n"
        "Subscriber waits for a proxy; restart Subscriber explicitly after source identity changes.\n";
}
const char* State(n::TopicRouteState s) {
    switch(s) {
        case n::TopicRouteState::ACTIVE:return "ACTIVE";
        case n::TopicRouteState::WAITING_SUBSCRIBER:return "WAITING_SUBSCRIBER";
        case n::TopicRouteState::REJECTED:return "REJECTED";
        case n::TopicRouteState::CLOSED:return "CLOSED";
    }
    return "UNKNOWN";
}
class Element : public kcf::ProcessElement {
public:
    Element(bool publisher, n::ControlConfig config):publishing_(publisher),config_(std::move(config)) {}
    int Setup() override {
        config_.metadata.runtime_pids={getpid()};
        if (publishing_) {
            int code=publisher_.Create(Topic,8);
            if (code) return Error("Publisher Create",code);
            created_=true;
        }
        int code=node_.Start(config_);
        if (code) return Error("Gateway Start",code);
        std::cout << "Ready mode=" << (publishing_?"publisher":"subscriber")
            << " topic=" << Topic << " diagnostics=ON\n";
        report_=Clock::now();
        return 0;
    }
    int Loop() override {
        int code=node_.GetLastError();
        if (code) return Error("Gateway worker",code);
        if (publishing_) {
            if (published_==static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                return Error("application sequence exhausted",-EOVERFLOW);
            auto seq=static_cast<std::int32_t>(published_+1);
            code=publisher_.Publish({seq,true,seq*2.0});
            if (!code) ++published_;
            else if (code==-EAGAIN) ++publish_busy_;
            else return Error("Publish",code);
        }
        auto now=Clock::now();
        // Diagnostics/Proxy reconciliation at 10 Hz, not on every 200 Hz cycle.
        if (now>=inspect_) {
            inspect_=now+std::chrono::milliseconds(100);
            auto diagnostics=node_.GetTopicDiagnostics();
            if (!publishing_) {
                for (const auto& proxy:diagnostics.proxies) {
                    if (proxy.name!=Topic) continue;
                    if (subscribed_ && proxy.source!=source_) {
                        std::cerr << "Source identity changed: explicitly restart subscriber (new sequence epoch).\n";
                        return -ESTALE;
                    }
                    if (!subscribed_) {
                        code=subscriber_.Create(Topic,[this](const Value& v) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            ++received_;
                            if (received_>1) { if (v.count==latest_) ++duplicates_; else if (v.count<latest_) ++backwards_; }
                            if (v.count<=0||!v.enabled||v.gain!=v.count*2.0) ++invalid_;
                            latest_=v.count;
                        });
                        if (code) return Error("Subscriber Create after proxy",code);
                        source_=proxy.source; subscribed_=true;
                        std::cout << "Proxy ready; Subscriber callback attached\n";
                    }
                }
            }
            if (now>=report_) {
                report_=now+std::chrono::seconds(1);
                Print(diagnostics);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (duplicates_||backwards_||invalid_) return Error("payload validation",-EPROTO);
        }
        return 0;
    }
    void Shutdown() override {
        // Callback joins before counters/Element are destroyed; gateway stops afterwards.
        if (subscribed_) {
            int code=subscriber_.Close();
            if (code) Error("Subscriber Close",code);
            subscribed_=false;
        }
        Print(node_.GetTopicDiagnostics());
        node_.Stop();
        if (created_) {
            int code=publisher_.Unlink(); if (code) Error("Publisher Unlink",code);
            code=publisher_.Close(); if (code) Error("Publisher Close",code);
            created_=false;
        }
        std::cout << "Shutdown complete\n";
    }
private:
    int Error(const char* stage,int code) { std::cerr << stage << " error=" << code << '\n'; return code; }
    void Print(const n::TopicDiagnostics& d) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "Status published=" << published_ << " publish_busy=" << publish_busy_
                << " received=" << received_ << " latest=" << latest_ << " duplicate=" << duplicates_
                << " backwards=" << backwards_ << " invalid=" << invalid_
                << " send_routes=" << d.active_send_routes << " receive_routes=" << d.active_receive_routes << '\n';
        }
        for (const auto& peer:node_.GetPeers())
            std::cout << "Peer address=" << peer.address << " state=" << (peer.state==n::PeerState::ONLINE?"ONLINE":"LOST")
                << " session=" << peer.session_id << " compatibility=" << n::CompatibilityReasonName(peer.compatibility.reason) << '\n';
        for (const auto& r:d.routes)
            std::cout << "Route " << State(r.state) << " sending=" << r.sending << " id=" << r.route_id
                << " source_session=" << r.source_session << " destination_session=" << r.destination_session
                << " sent=" << r.packets_sent << " received=" << r.packets_received << " proxy_published=" << r.samples_published
                << " skipped=" << r.local_samples_skipped << " dropped=" << r.dropped << " duplicate=" << r.duplicate
                << " out_of_order=" << r.out_of_order << " missed=" << r.missed_sequence << " malformed=" << r.malformed
                << " error=" << r.error << " reason=" << n::CompatibilityReasonName(r.reason) << '\n';
    }
    bool publishing_,created_{false},subscribed_{false};
    n::ControlConfig config_;
    kcf::Publisher<Value> publisher_;
    kcf::Subscriber<Value> subscriber_;
    n::ControlNode node_;
    n::RemoteEndpointIdentity source_;
    Clock::time_point report_{},inspect_{};
    std::mutex mutex_;
    std::uint64_t published_{0},publish_busy_{0},received_{0},duplicates_{0},backwards_{0},invalid_{0};
    std::int32_t latest_{0};
};
}
int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc==2&&std::string(argv[1])=="--help") { Usage(); return 0; }
    n::ControlConfig config; bool publisher=false;
    try {
        if (argc!=7) throw std::invalid_argument("expected six arguments");
        std::string mode=argv[1];
        if (mode!="publisher"&&mode!="subscriber") throw std::invalid_argument("invalid mode");
        publisher=mode=="publisher";
        if (!Id(argv[2],config.discovery.host)) throw std::invalid_argument("invalid Host ID");
        in_addr address{};
        if (inet_pton(AF_INET,argv[3],&address)!=1||!ntohl(address.s_addr)||ntohl(address.s_addr)>=0xe0000000u)
            throw std::invalid_argument("expected local unicast IPv4");
        config.discovery.interface_address=argv[3];
        config.discovery.port=Port(argv[4]);config.discovery.metadata_port=Port(argv[5]);config.topics.port=Port(argv[6]);
        if (config.discovery.port==config.discovery.metadata_port||config.discovery.port==config.topics.port||config.discovery.metadata_port==config.topics.port)
            throw std::invalid_argument("all three ports must differ");
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; Usage(); return 2; }
    std::string boot;std::ifstream("/proc/sys/kernel/random/boot_id")>>boot;
    if (!Id(boot,config.discovery.boot_id)) { std::cerr << "Cannot read Linux boot identity\n";return 1; }
    config.diagnostics_enabled=true;config.topics.enabled=true;config.topics.max_hz=50;
    config.metadata.remote_topics={Topic};
    config.topics.local_types={{Topic,kcf::TypeDescriptorTraits<Value>::Get()}};
    Element element(publisher,config);
    kcf::ProcessRuntime runtime;runtime.SetLoopFrequency(200);
    return runtime.Run(element)==0?0:1;
}
