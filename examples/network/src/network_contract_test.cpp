#include "kcf/network/control_codec.hpp"
#include "kcf/ipc/detail/storage_layout.hpp"
#include "kcf/introspection/runtime_info.hpp"
#include "kcf/introspection/service_info.hpp"
#include "kcf/service/service_protocol.hpp"
#include <cassert>
#include <iostream>

namespace n = kcf::network;
n::HostIdentity Host(unsigned char value) { n::HostIdentity h; h.bytes[0] = value; return h; }
n::RemoteRuntimeIdentity Runtime(unsigned char host = 1) { return {Host(host), Host(99), 42, 1000}; }
n::TypeIdentity Type() { return {10, 20, 1, 4}; }
n::NetworkEndpointMetadata Endpoint(n::EndpointKind kind, unsigned char host = 1)
{
    n::NetworkEndpointMetadata m;
    m.identity = {Runtime(host), 1};
    m.name = "/test"; m.element_name = "element";
    m.scope = n::NetworkScope::REMOTE; m.kind = kind;
    m.application = Runtime(host); m.application.pid = 7; m.application_name = "app";
    m.groups = {"mobile_robots"};
    if (kind == n::EndpointKind::TOPIC || kind == n::EndpointKind::PARAMETER) m.value_type = Type();
    else { m.request_type = Type(); m.response_type = Type(); }
    if (kind == n::EndpointKind::ACTION) m.feedback_type = Type();
    return m;
}
n::RemoteRequest Request()
{
    n::RemoteRequest r;
    r.source = Runtime(9); r.request_id = 100; r.timeout_ms = 200;
    r.target.scope = n::TargetScope::ALL;
    r.endpoint_name = "/test"; r.operation = n::ControlOperation::SERVICE_REQUEST;
    r.payload_type = Type(); r.response_type = Type(); r.payload = {1, 2, 3, 4};
    return r;
}
void Identities()
{
    assert(!n::Valid(n::HostIdentity{}));
    auto a = Runtime(), b = a;
    assert(n::Valid(a) && a == b);
    b.host = Host(2); assert(a != b); // identical PID/ticks, different host
    b = a; b.boot_id = Host(3); assert(a != b); // same device after reboot
    b = a; ++b.process_start_ticks; assert(a != b);
    b = a; ++b.pid; assert(a != b);
    b = a; b.pid = 0; assert(!n::Valid(b));
    b = a; b.process_start_ticks = 0; assert(!n::Valid(b));
    b = a; b.boot_id = {}; assert(!n::Valid(b));
    n::RemoteEndpointIdentity x{a, 1}, y{a, 2};
    assert(x != y && n::Valid(x)); y.registration_id = 0; assert(!n::Valid(y));
}
void TargetsAndMetadata()
{
    const auto endpoint = Endpoint(n::EndpointKind::SERVICE);
    n::RemoteTarget t;
    t.endpoint = endpoint.identity; assert(n::Valid(t) && n::MatchesTarget(t, endpoint));
    t.host = Host(1); assert(!n::Valid(t)); // conflicting selectors
    t = {}; t.scope = n::TargetScope::HOST; t.host = Host(1);
    assert(n::Valid(t) && n::MatchesTarget(t, endpoint));
    t.host = Host(2); assert(!n::MatchesTarget(t, endpoint));
    t = {}; t.scope = n::TargetScope::APPLICATION; t.application = endpoint.application;
    assert(n::Valid(t) && n::MatchesTarget(t, endpoint));
    ++t.application.process_start_ticks; assert(!n::MatchesTarget(t, endpoint));
    t = {}; t.scope = n::TargetScope::GROUP; t.group = "mobile_robots";
    assert(n::Valid(t) && n::MatchesTarget(t, endpoint));
    t.group = "other"; assert(!n::MatchesTarget(t, endpoint));
    t.group = std::string("bad\0group", 9); assert(!n::Valid(t));
    t = {}; t.scope = n::TargetScope::ALL; assert(n::Valid(t) && n::MatchesTarget(t, endpoint));
    t.scope = static_cast<n::TargetScope>(99); assert(!n::Valid(t));
    for (int i = 0; i < 11; ++i) {
        auto m = endpoint;
        switch (i) {
        case 0: m.protocol_version = 0; break;
        case 1: m.identity.runtime.host = {}; break;
        case 2: m.kind = static_cast<n::EndpointKind>(99); break;
        case 3: m.role = static_cast<n::EndpointRole>(99); break;
        case 4: m.scope = static_cast<n::NetworkScope>(99); break;
        case 5: m.request_type.schema_id = 0; break;
        case 6: m.name = "not_absolute"; break;
        case 7: m.application.host = Host(2); break;
        case 8: m.groups.push_back(m.groups[0]); break;
        case 9: m.feedback_type = Type(); break;
        case 10: m.request_type.payload_size = n::MAX_CONTROL_PAYLOAD + 1; break;
        }
        assert(!n::Valid(m));
    }
    auto standalone = endpoint; standalone.application = {}; standalone.application_name.clear();
    assert(n::Valid(standalone)); standalone.application_name = "fake"; assert(!n::Valid(standalone));
    auto local = endpoint; local.scope = n::NetworkScope::LOCAL;
    t = {}; t.scope = n::TargetScope::ALL; assert(!n::MatchesTarget(t, local));
}
void PolicyAndCodec()
{
    n::NetworkEndpointMetadata defaults; assert(defaults.scope == n::NetworkScope::LOCAL);
    auto pub = Endpoint(n::EndpointKind::TOPIC), sub = Endpoint(n::EndpointKind::TOPIC, 2);
    sub.role = n::EndpointRole::CONSUMER;
    assert(n::CanRouteTopic(pub, sub, true));
    assert(!n::CanRouteTopic(pub, sub, false));
    pub.scope = n::NetworkScope::LOCAL; assert(!n::CanRouteTopic(pub, sub, true));
    pub.scope = n::NetworkScope::REMOTE;
    sub.identity.runtime.host = pub.identity.runtime.host;
    sub.application.host = pub.identity.runtime.host;
    assert(n::Valid(sub) && !n::CanRouteTopic(pub, sub, true));
    sub = Endpoint(n::EndpointKind::TOPIC, 2); sub.role = n::EndpointRole::CONSUMER;
    sub.scope = n::NetworkScope::LOCAL; assert(!n::CanRouteTopic(pub, sub, true));
    sub.scope = n::NetworkScope::REMOTE; sub.name = "/different"; assert(!n::CanRouteTopic(pub, sub, true));
    sub.name = pub.name; sub.value_type.schema_id++; assert(!n::CanRouteTopic(pub, sub, true));
    n::Priority priority{};
    assert(n::Classify(n::TrafficClass::TOPIC, priority) && priority == n::Priority::NORMAL);
    assert(n::Classify(n::TrafficClass::CONTROL, priority) && priority == n::Priority::HIGH);
    assert(!n::Classify(static_cast<n::TrafficClass>(99), priority) && priority == n::Priority::HIGH);
    for (unsigned i = 1; i <= 8; ++i) {
        auto operation = static_cast<n::ControlOperation>(i);
        assert(n::Classify(operation, priority) && priority == n::Priority::HIGH);
        std::array<std::uint8_t, 2> bytes{};
        assert(n::EncodeControlOperation(operation, bytes) == 0 && bytes[0] == 0 && bytes[1] == i);
        n::ControlOperation decoded{};
        assert(n::DecodeControlOperation(bytes.data(), bytes.size(), decoded) == 0 && decoded == operation);
    }
    n::ControlOperation out = n::ControlOperation::PARAM_GET;
    std::array<std::uint8_t, 2> bytes{99, 99};
    assert(n::EncodeControlOperation(static_cast<n::ControlOperation>(99), bytes) == -EINVAL && bytes[0] == 99);
    assert(n::DecodeControlOperation(bytes.data(), 2, out) == -EPROTO && out == n::ControlOperation::PARAM_GET);
    assert(n::DecodeControlOperation(bytes.data(), 1, out) == -EMSGSIZE);
    assert(n::DecodeControlOperation(nullptr, 2, out) == -EINVAL);
    // Exhaust all operation tags: only the eight assigned values may decode.
    for (unsigned i = 0; i < 65536; ++i) {
        bytes = {static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i)};
        assert((n::DecodeControlOperation(bytes.data(), 2, out) == 0) == (i >= 1 && i <= 8));
    }
}
void RequestsAndResults()
{
    auto r = Request(); auto a = Endpoint(n::EndpointKind::SERVICE), b = Endpoint(n::EndpointKind::SERVICE, 2);
    assert(n::Valid(r) && n::MatchesRequest(r, a) && n::MatchesRequest(r, b));
    n::RemoteRequestResult results;
    results.endpoints = {{r.source, r.request_id, a.identity, n::RemoteStatus::SUCCESS, 0, Type(), {1,2,3,4}},
                         {r.source, r.request_id, b.identity, n::RemoteStatus::TIMEOUT, 0, {}, {}}};
    assert(n::Valid(results, r, {a,b}));
    std::swap(results.endpoints[0], results.endpoints[1]); assert(n::Valid(results, r, {a,b}));
    assert(results.endpoints[0].status == n::RemoteStatus::TIMEOUT && results.endpoints[1].payload[0] == 1);
    auto saved = results;
    results.endpoints.pop_back(); assert(!n::Valid(results, r, {a,b})); results = saved;
    results.endpoints[0].endpoint = a.identity; assert(!n::Valid(results, r, {a,b})); results = saved;
    results.endpoints[0].source.host = Host(88); assert(!n::Valid(results, r, {a,b})); results = saved;
    ++results.endpoints[0].request_id; assert(!n::Valid(results, r, {a,b})); results = saved;
    results.endpoints[1].payload_type.schema_id++; assert(!n::Valid(results, r, {a,b})); results = saved;
    results.endpoints[0].status = static_cast<n::RemoteStatus>(99); assert(!n::Valid(results, r, {a,b})); results = saved;
    results.endpoints[0].payload = {1}; assert(!n::Valid(results, r, {a,b})); results = saved;
    r.target.scope = n::TargetScope::HOST; r.target.host = a.identity.runtime.host;
    assert(!n::Valid(results, r, {a,b})); r = Request();
    results.endpoints[0].status = n::RemoteStatus::REMOTE_ERROR;
    assert(!n::Valid(results, r, {a,b})); results.endpoints[0].error = -EFAULT; assert(n::Valid(results, r, {a,b}));
    assert(!n::Valid(results, r, {a,a}) && !n::Valid(n::RemoteRequestResult{}, r, {}));
    for (auto status : {n::RemoteStatus::PENDING, n::RemoteStatus::DISCONNECTED, n::RemoteStatus::LOST}) {
        results = saved; results.endpoints[0].status = status; assert(n::Valid(results, r, {a,b}));
    }
    for (int i = 0; i < 10; ++i) {
        auto bad = r;
        switch (i) {
        case 0: bad.request_id = 0; break;
        case 1: bad.protocol_version++; break;
        case 2: bad.source.pid = -1; break;
        case 3: bad.payload.pop_back(); break;
        case 4: bad.timeout_ms = 0; break;
        case 5: bad.operation = n::ControlOperation::SERVICE_RESPONSE; break;
        case 6: bad.response_type.encoding_id = 0; break;
        case 7: bad.target.scope = static_cast<n::TargetScope>(99); break;
        case 8: bad.endpoint_name = std::string(241, '/'); break;
        case 9: bad.goal = {bad.source, 10}; break;
        }
        assert(!n::Valid(bad));
    }
    r.response_type.schema_id++; assert(!n::MatchesRequest(r, a));
    r = Request(); r.operation = n::ControlOperation::PARAM_SET; r.response_type = {};
    assert(n::MatchesRequest(r, Endpoint(n::EndpointKind::PARAMETER)));
    r.operation = n::ControlOperation::PARAM_GET; r.payload.clear(); r.payload_type = {}; r.response_type = Type();
    assert(n::MatchesRequest(r, Endpoint(n::EndpointKind::PARAMETER)));
    r = Request(); r.operation = n::ControlOperation::ACTION_GOAL;
    a = Endpoint(n::EndpointKind::ACTION); assert(n::MatchesRequest(r, a));
    n::RemoteActionEvent event{{r.source, r.request_id}, a.identity, n::ControlOperation::ACTION_FEEDBACK, Type(), {1,2,3,4}};
    assert(n::Valid(event, a)); event.operation = n::ControlOperation::ACTION_RESULT; assert(n::Valid(event, a));
    event.payload_type.type_id++; assert(!n::Valid(event, a));
    r.operation = n::ControlOperation::ACTION_CANCEL; r.payload.clear(); r.payload_type = {}; r.response_type = {};
    assert(!n::Valid(r)); r.goal = {r.source, 100}; r.request_id = 101;
    assert(n::Valid(r) && n::MatchesRequest(r, a));
    r.operation = n::ControlOperation::ACTION_RESULT; r.response_type = Type(); assert(n::Valid(r));
}
int main()
{
    static_assert(kcf::detail::TOPIC_STORAGE_FORMAT == 4);
    static_assert(kcf::detail::STORAGE_FORMAT == 3);
    static_assert(kcf::SERVICE_PROTOCOL_VERSION == 2 && sizeof(kcf::ServiceHeader) == 72);
    static_assert(sizeof(kcf::ServiceInfo) == 600);
    static_assert(offsetof(kcf::RuntimeInfo, pid) == 8 && offsetof(kcf::RuntimeInfo, process_start_ticks) == 16);
    Identities(); TargetsAndMetadata(); PolicyAndCodec(); RequestsAndResults();
    std::cout << "Network contracts PASS: host/boot identities, five targets, metadata, policy, priority, operation codec, per-endpoint results, Action correlation, Local format guards\n";
}
