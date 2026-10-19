#include "kcf/network/control_codec.hpp"
#include "kcf/network/network_diagnostics.hpp"
#include "kcf/network/control_frame.hpp"
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

void DiagnosticsCodec()
{
    static_assert(n::NETWORK_DIAGNOSTICS_VERSION==1 && n::MAX_NETWORK_DIAGNOSTICS_PAYLOAD==16384);
    static_assert(n::DISCOVERY_VERSION==2 && n::CONTROL_WIRE_VERSION==1 && n::TOPIC_DATA_VERSION==1);
    const n::NetworkProtocolVersion protocol;assert(protocol.major==1 && protocol.minor==0);
    n::TopicDiagnostics diagnostics;
    n::NetworkDiagnosticsSnapshot snapshot, decoded;
    std::vector<std::uint8_t> bytes, again;
    const auto make=[&] {assert(n::MakeNetworkDiagnosticsSnapshot(Host(1),Host(99),0x0102030405060708ULL,diagnostics,snapshot)==0);};
    const auto roundtrip=[&] {
        assert(n::EncodeNetworkDiagnosticsSnapshot(snapshot,bytes)==0);
        assert(bytes.size()<=16384);
        assert(n::DecodeNetworkDiagnosticsSnapshot(bytes.data(),bytes.size(),decoded)==0);
        assert(n::EncodeNetworkDiagnosticsSnapshot(decoded,again)==0 && bytes==again);
        assert(decoded.gateway_host==snapshot.gateway_host && decoded.gateway_boot_id==snapshot.gateway_boot_id);
        assert(decoded.gateway_session==snapshot.gateway_session && decoded.total_routes==snapshot.total_routes);
    };
    make();roundtrip();assert(bytes.size()==60 && decoded.routes.empty() && !decoded.truncated);
    assert(bytes[0]=='K' && bytes[1]=='N' && bytes[2]=='D' && bytes[3]=='1');
    assert(bytes[4]==0 && bytes[5]==1 && bytes[10]==0 && bytes[11]==60);
    for(unsigned i=0;i<8;++i)assert(bytes[44+i]==i+1); // independently pin BE session bytes
    n::TopicRouteInfo route;route.source={Runtime(1),11};route.destination={Runtime(2),22};
    route.source_session=31;route.destination_session=32;route.route_id=0x1122334455667788ULL;
    route.name="/diagnostics";route.state=n::TopicRouteState::ACTIVE;route.sending=true;
    route.error=-123;route.reason=n::CompatibilityReason::TYPE_MISMATCH;
    route.proxy_recovery=n::ProxyRecoveryState::RECOVERED;
    route.packets_sent=UINT64_MAX;route.packets_received=2;route.samples_published=3;
    route.dropped=4;route.duplicate=5;route.out_of_order=6;route.missed_sequence=7;
    route.malformed=8;route.local_samples_skipped=9;
    diagnostics.routes={route};make();roundtrip();
    const auto& result=decoded.routes.at(0);
    assert(result.source==route.source && result.destination==route.destination && result.name==route.name);
    assert(result.source_session==31 && result.destination_session==32 && result.route_id==route.route_id);
    assert(result.state==route.state && result.sending && result.error==-123 && result.reason==route.reason);
    assert(result.proxy_recovery==route.proxy_recovery);
    assert(result.packets_sent==UINT64_MAX && result.packets_received==2 && result.samples_published==3);
    assert(result.dropped==4 && result.duplicate==5 && result.out_of_order==6 && result.missed_sequence==7);
    assert(result.malformed==8 && result.local_samples_skipped==9);
    for(unsigned i=0;i<8;++i)assert(bytes[180+i]==0x11*(i+1)); // route ID BE
    assert(bytes[192]==255 && bytes[193]==255 && bytes[194]==255 && bytes[195]==133); // -123
    const auto good=bytes;
    // Invalid decoding is atomic and never leaves a partial result in output.
    const auto reject=[&](const std::vector<std::uint8_t>& bad) {
        auto saved=decoded;std::vector<std::uint8_t> before,after;
        assert(n::EncodeNetworkDiagnosticsSnapshot(saved,before)==0);
        assert(n::DecodeNetworkDiagnosticsSnapshot(bad.data(),bad.size(),decoded)<0);
        assert(n::EncodeNetworkDiagnosticsSnapshot(decoded,after)==0 && before==after);
    };
    for(std::size_t length=0;length<good.size();++length)
        reject(std::vector<std::uint8_t>(good.begin(),good.begin()+length));
    auto bad=good;bad.push_back(0);reject(bad);
    bad[8]=0;bad[9]=0;bad[10]=static_cast<std::uint8_t>(bad.size()>>8);bad[11]=static_cast<std::uint8_t>(bad.size());reject(bad); // trailing byte with matching size
    reject(std::vector<std::uint8_t>(16385,0));
    for(auto entry:std::vector<std::pair<std::size_t,std::uint8_t>>{
        {0,0},{5,2},{7,2},{11,0},{55,0},{59,2}, // magic, version, flags, size, total, count
        {188,4},{189,2},{190,255},{191,255}, // state/bool/reason/recovery
        {92,128},{112+32,128},{268,1},{269,0},{270,'x'},{270,0}}) {
        bad=good;bad[entry.first]=entry.second;reject(bad);
    }
    // Required identity components, including malformed partly empty identities.
    for(auto range:std::vector<std::pair<std::size_t,std::size_t>>{
        {12,16},{28,16},{44,8},{60,16},{76,16},{92,4},{96,8},{104,8},
        {112,16},{128,16},{144,4},{148,8},{156,8},{164,8},{172,8},{180,8}}) {
        bad=good;std::fill(bad.begin()+range.first,bad.begin()+range.first+range.second,0);reject(bad);
    }
    assert(n::DecodeNetworkDiagnosticsSnapshot(nullptr,60,decoded)==-EINVAL);
    // Each state, both directions, full signed error range and valid notice forms.
    for(auto state:{n::TopicRouteState::WAITING_SUBSCRIBER,n::TopicRouteState::ACTIVE,
                   n::TopicRouteState::REJECTED,n::TopicRouteState::CLOSED}) {
        diagnostics.routes={route};diagnostics.routes[0].state=state;
        diagnostics.routes[0].sending=false;diagnostics.routes[0].error=INT32_MIN;
        make();roundtrip();assert(decoded.routes[0].error==INT32_MIN && !decoded.routes[0].sending);
    }
    for(auto state:{n::TopicRouteState::WAITING_SUBSCRIBER,n::TopicRouteState::REJECTED}) {
        n::TopicRouteInfo notice;notice.source=route.source;notice.name=route.name;notice.state=state;
        notice.error=-ENOENT;notice.proxy_recovery=n::ProxyRecoveryState::LOCAL_PUBLISHER;
        diagnostics.routes={notice};make();roundtrip();assert(decoded.routes[0].destination==n::RemoteEndpointIdentity{});
    }
    // Largest legal payload: 60 + 37*210 + (1 + 153 + 35*240) = 16384.
    route.name="/"+std::string(239,'a');diagnostics.routes.assign(37,route);
    diagnostics.routes[0].name="/";diagnostics.routes[1].name="/"+std::string(152,'b');
    make();roundtrip();assert(bytes.size()==16384 && decoded.routes.size()==37 && !decoded.truncated);
    diagnostics.routes.push_back(route);make();roundtrip();
    assert(bytes.size()==16384 && decoded.total_routes==38 && decoded.routes.size()==37 && decoded.truncated);
    // Encoder itself also enforces the cap on a manually constructed complete snapshot.
    snapshot.routes=diagnostics.routes;snapshot.truncated=false;roundtrip();
    assert(decoded.total_routes==38 && decoded.truncated && decoded.routes.size()==37);
    bad=bytes;bad[7]=0;reject(bad); // truncated flag must agree with total/included
    // An already truncated response can have zero included routes.
    snapshot.routes.clear();snapshot.truncated=true;roundtrip();assert(decoded.routes.empty() && decoded.truncated);
    // Invalid input, including omitted routes, must not modify either output.
    const auto saved_bytes=bytes;const auto saved_total=decoded.total_routes;
    diagnostics.routes.back().source={};
    assert(n::MakeNetworkDiagnosticsSnapshot(Host(1),Host(99),1,diagnostics,decoded)==-EINVAL);
    assert(decoded.total_routes==saved_total);
    snapshot.routes=diagnostics.routes;snapshot.truncated=false;
    assert(n::EncodeNetworkDiagnosticsSnapshot(snapshot,bytes)==-EINVAL && bytes==saved_bytes);
    diagnostics.routes={route};make();
    for(int i=0;i<9;++i) {
        auto invalid=snapshot;
        switch(i) {
        case 0: invalid.gateway_host={};break;
        case 1: invalid.gateway_boot_id={};break;
        case 2: invalid.gateway_session=0;break;
        case 3: invalid.total_routes=0;break;
        case 4: invalid.truncated=true;break;
        case 5: invalid.routes[0].name=std::string(241,'/');break;
        case 6: invalid.routes[0].destination={};break; // ACTIVE cannot be a notice
        case 7: invalid.routes[0].state=static_cast<n::TopicRouteState>(255);break;
        case 8: invalid.routes[0].name="/bad\nname";break;
        }
        assert(n::EncodeNetworkDiagnosticsSnapshot(invalid,bytes)==-EINVAL && bytes==saved_bytes);
    }
    std::cout << "Network diagnostics snapshot PASS: empty, fields/counters, BE, 16 KiB, truncation, notices, malformed and atomic outputs\n";
}


void DiagnosticsFrames()
{
    static_assert(static_cast<unsigned>(n::ControlMessage::HELLO)==1);
    static_assert(static_cast<unsigned>(n::ControlMessage::ACTION_CANCEL_RESPONSE)==13);
    static_assert(static_cast<unsigned>(n::ControlMessage::DIAGNOSTICS_REQUEST)==14);
    static_assert(static_cast<unsigned>(n::ControlMessage::DIAGNOSTICS_RESPONSE)==15);
    static_assert(n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS)==(1ull<<10));
    static_assert(n::KNOWN_CAPABILITIES==((1ull<<11)-1));
    assert(!(n::PeerProfile{}.capabilities & n::CapabilityBit(n::Capability::NETWORK_DIAGNOSTICS)));
    n::ControlFrame request;request.operation=n::ControlMessage::DIAGNOSTICS_REQUEST;
    request.source=Runtime(9);request.session_id=1;request.request_id=0x01020304;
    request.target=n::TargetScope::HOST;request.timeout_ms=200;
    n::ControlFrame decoded;std::vector<std::uint8_t> bytes;
    const auto roundtrip=[&](const n::ControlFrame& f) {
        assert(n::EncodeControlFrame(f,bytes)==0);
        std::size_t size=0;assert(n::ControlFrameSize(bytes.data(),16,size)==0 && size==bytes.size());
        assert(n::DecodeControlFrame(bytes.data(),bytes.size(),decoded)==0);
        assert(decoded.operation==f.operation && decoded.source==f.source && decoded.session_id==f.session_id);
        assert(decoded.request_id==f.request_id && decoded.timeout_ms==f.timeout_ms && decoded.status==f.status);
        assert(decoded.target==f.target && decoded.reason==f.reason && decoded.payload==f.payload);
        std::vector<std::uint8_t> again;assert(n::EncodeControlFrame(decoded,again)==0 && again==bytes);
    };
    roundtrip(request);assert(bytes.size()==172 && bytes[6]==0 && bytes[7]==14 && bytes[5]==1);
    assert(bytes[72]==1 && bytes[73]==2 && bytes[74]==3 && bytes[75]==4);
    const auto request_wire=bytes;
    for(auto timeout:{1u,60000u}){auto f=request;f.timeout_ms=timeout;roundtrip(f);}
    n::NetworkDiagnosticsSnapshot snapshot;
    assert(!n::MakeNetworkDiagnosticsSnapshot(Host(1),Host(99),10,{},snapshot));
    auto response=request;response.operation=n::ControlMessage::DIAGNOSTICS_RESPONSE;
    response.source=Runtime(1);response.session_id=10;response.timeout_ms=0;
    assert(!n::EncodeNetworkDiagnosticsSnapshot(snapshot,response.payload));roundtrip(response);
    assert(bytes.size()==172+60 && bytes[7]==15 && decoded.request_id==request.request_id);
    const auto response_wire=bytes;
    auto failure=response;failure.status=-EACCES;failure.reason=n::CompatibilityReason::CAPABILITY_MISSING;
    failure.payload.clear();roundtrip(failure);assert(bytes.size()==172 && decoded.status==-EACCES);
    const auto reject_frame=[&](const n::ControlFrame& f) {
        const auto saved=bytes;assert(n::EncodeControlFrame(f,bytes)==-EINVAL && bytes==saved);
    };
    for(int i=0;i<19;++i) {
        auto f=request;
        switch(i) {
        case 0:f.source={};break;case 1:f.session_id=0;break;case 2:f.request_id=0;break;
        case 3:f.timeout_ms=0;break;case 4:f.timeout_ms=60001;break;
        case 5:f.endpoint={Runtime(2),1};break;case 6:f.name="/forbidden";break;
        case 7:f.type=Type();break;case 8:f.payload={1};break;
        case 9:f.response_type=Type();break;case 10:f.feedback_type=Type();break;
        case 11:f.goal={Runtime(),1,1};break;case 12:f.action_state=kcf::ActionState::RUNNING;break;
        case 13:f.feedback_sequence=1;break;case 14:f.outcome_unknown=true;break;
        case 15:f.target=n::TargetScope::ALL;break;case 16:f.status=-EIO;break;
        case 17:f.reason=n::CompatibilityReason::TYPE_MISMATCH;break;
        case 18:f.target=static_cast<n::TargetScope>(255);break;
        }
        reject_frame(f);
    }
    for(int i=0;i<11;++i) {
        auto f=response;
        switch(i) {
        case 0:f.status=1;break;case 1:f.status=-EIO;break; // failure must be empty
        case 2:f.payload.clear();break;case 3:f.timeout_ms=1;break;
        case 4:f.reason=static_cast<n::CompatibilityReason>(255);break;
        case 5:f.reason=n::CompatibilityReason::TYPE_MISMATCH;break;
        case 6:f.payload[5]=2;break;case 7:f.source.host=Host(3);break;
        case 8:f.source.boot_id=Host(3);break;case 9:++f.session_id;break;
        case 10:f.request_id=0;break;
        }
        reject_frame(f);
    }
    const auto reject_wire=[&](const std::vector<std::uint8_t>& bad) {
        std::vector<std::uint8_t> before,after;assert(!n::EncodeControlFrame(decoded,before));
        assert(n::DecodeControlFrame(bad.data(),bad.size(),decoded)<0);
        assert(!n::EncodeControlFrame(decoded,after) && before==after);
    };
    for(const auto& wire:{request_wire,response_wire}) {
        for(std::size_t length=0;length<wire.size();++length)
            reject_wire(std::vector<std::uint8_t>(wire.begin(),wire.begin()+length));
        auto bad=wire;bad.push_back(0);reject_wire(bad);
        for(auto offset:{0,5,7,11,15,48,76,128,129,130,139,164,166,168}) {
            bad=wire;bad[offset]=255;reject_wire(bad);
        }
    }
    auto bad=request_wire;bad[16]=0;reject_wire(bad); // zero source host
    bad=response_wire;bad[177]=2;reject_wire(bad); // F-ND1 version
    bad=response_wire;bad[172+12]=2;reject_wire(bad); // valid snapshot, wrong gateway host
    // Pin the 16 KiB payload boundary, using the F-ND1 codec rather than a copy.
    n::TopicRouteInfo route;route.source={Runtime(1),1};route.destination={Runtime(2),2};
    route.source_session=10;route.destination_session=20;route.route_id=30;
    route.state=n::TopicRouteState::ACTIVE;route.name="/"+std::string(239,'x');
    n::TopicDiagnostics diagnostics;diagnostics.routes.assign(37,route);
    diagnostics.routes[0].name="/";diagnostics.routes[1].name="/"+std::string(152,'x');
    assert(!n::MakeNetworkDiagnosticsSnapshot(Host(1),Host(99),10,diagnostics,snapshot));
    assert(!n::EncodeNetworkDiagnosticsSnapshot(snapshot,response.payload));
    roundtrip(response);assert(bytes.size()==172+16384);
    response.payload.push_back(0);reject_frame(response);
    // Reject oversize Diagnostics from the prefix before a listener allocates a body.
    bad=bytes;bad[8]=0;bad[9]=0;bad[10]=static_cast<std::uint8_t>((172+16385)>>8);bad[11]=static_cast<std::uint8_t>(172+16385);
    std::size_t length=99;assert(n::ControlFrameSize(bad.data(),16,length)==-EMSGSIZE && length==99);
    bad=request_wire;bad[11]=173;assert(n::ControlFrameSize(bad.data(),16,length)==-EMSGSIZE);
    // Existing HELLO envelope remains 172 bytes, without a diagnostics extension.
    n::ControlFrame hello;hello.source=Runtime(1);hello.session_id=1;roundtrip(hello);
    assert(bytes.size()==172 && bytes[7]==1 && bytes[5]==1);
    std::cout << "Diagnostics Control v1 PASS: tags, capability contract, requests/responses, snapshot validation, 16 KiB bound and malformed frames\n";
}

int main()
{
    static_assert(kcf::detail::TOPIC_STORAGE_FORMAT == 4);
    static_assert(kcf::detail::STORAGE_FORMAT == 3);
    static_assert(kcf::SERVICE_PROTOCOL_VERSION == 2 && sizeof(kcf::ServiceHeader) == 72);
    static_assert(sizeof(kcf::ServiceInfo) == 600);
    static_assert(offsetof(kcf::RuntimeInfo, pid) == 8 && offsetof(kcf::RuntimeInfo, process_start_ticks) == 16);
    Identities(); TargetsAndMetadata(); PolicyAndCodec(); RequestsAndResults(); DiagnosticsCodec(); DiagnosticsFrames();
    std::cout << "Network contracts PASS: host/boot identities, five targets, metadata, policy, priority, operation codec, per-endpoint results, Action correlation, Local format guards\n";
}
