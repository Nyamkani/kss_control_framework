#include "kcf/network/network_diagnostics.hpp"
#include <cerrno>
#include <limits>

namespace kcf::network {
namespace {
constexpr std::uint32_t MAGIC = 0x4b4e4431;
constexpr std::size_t HEADER = 60, ROUTE_FIXED = 210;
constexpr std::uint64_t TopicRouteInfo::* COUNTERS[] = {
    &TopicRouteInfo::packets_sent, &TopicRouteInfo::packets_received,
    &TopicRouteInfo::samples_published, &TopicRouteInfo::dropped,
    &TopicRouteInfo::duplicate, &TopicRouteInfo::out_of_order,
    &TopicRouteInfo::missed_sequence, &TopicRouteInfo::malformed,
    &TopicRouteInfo::local_samples_skipped
};
bool ValidRoute(const TopicRouteInfo& r) {
    if (!Valid(r.source) || !Text(r.name,240) || r.name[0]!='/' ||
        static_cast<unsigned>(r.state)>static_cast<unsigned>(TopicRouteState::CLOSED) ||
        static_cast<unsigned>(r.reason)>static_cast<unsigned>(CompatibilityReason::UNKNOWN_ENDPOINT) ||
        static_cast<unsigned>(r.proxy_recovery)>static_cast<unsigned>(ProxyRecoveryState::CLEANUP_FAILED) ||
        r.error<std::numeric_limits<std::int32_t>::min() || r.error>std::numeric_limits<std::int32_t>::max()) return false;
    if (r.destination==RemoteEndpointIdentity{})
        return (r.state==TopicRouteState::WAITING_SUBSCRIBER || r.state==TopicRouteState::REJECTED) &&
            !r.sending && !r.source_session && !r.destination_session && !r.route_id;
    return Valid(r.destination) && r.source.runtime.host!=r.destination.runtime.host &&
        r.source_session && r.destination_session && r.route_id;
}
bool ValidHeader(const NetworkDiagnosticsSnapshot& s) {
    return Valid(s.gateway_host) && Valid(s.gateway_boot_id) && s.gateway_session &&
        s.routes.size()<=s.total_routes && s.truncated==(s.routes.size()<s.total_routes);
}
// Keep a deterministic prefix; validate even omitted records to avoid hiding bad input.
int Prefix(const std::vector<TopicRouteInfo>& routes, std::size_t& count, std::size_t& size) {
    count=0;size=HEADER;bool full=false;
    for (const auto& r:routes) {
        if (!ValidRoute(r)) return -EINVAL;
        const auto bytes=ROUTE_FIXED+r.name.size();
        if (bytes>MAX_NETWORK_DIAGNOSTICS_PAYLOAD-size) full=true;
        if (!full) {size+=bytes;++count;}
    }
    return 0;
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    void U(std::uint64_t v,unsigned n) {while(n) bytes.push_back(static_cast<std::uint8_t>(v>>(8*--n)));}
    void Host(const HostIdentity& h) {for(auto b:h.bytes) U(b,1);}
    void Endpoint(const RemoteEndpointIdentity& e) {
        Host(e.runtime.host);Host(e.runtime.boot_id);U(e.runtime.pid,4);
        U(e.runtime.process_start_ticks,8);U(e.registration_id,8);
    }
};
struct Reader {
    const std::uint8_t* p;std::size_t left;bool ok{true};
    std::uint64_t U(unsigned n) {
        if(left<n) {ok=false;return 0;}
        std::uint64_t v=0;while(n--) {v=(v<<8)|*p++;--left;}return v;
    }
    HostIdentity Host() {HostIdentity h;for(auto& b:h.bytes)b=static_cast<std::uint8_t>(U(1));return h;}
    RemoteEndpointIdentity Endpoint() {
        RemoteEndpointIdentity e;e.runtime.host=Host();e.runtime.boot_id=Host();
        const auto pid=U(4);if(pid>INT32_MAX)ok=false;
        else e.runtime.pid=static_cast<std::int32_t>(pid);
        e.runtime.process_start_ticks=U(8);e.registration_id=U(8);return e;
    }
};
}
int MakeNetworkDiagnosticsSnapshot(const HostIdentity& host,const HostIdentity& boot,
    std::uint64_t session,const TopicDiagnostics& d,NetworkDiagnosticsSnapshot& output) {
    if (!Valid(host)||!Valid(boot)||!session) return -EINVAL;
    if (d.routes.size()>UINT32_MAX) return -EMSGSIZE;
    std::size_t count,size;const int result=Prefix(d.routes,count,size);if(result)return result;
    NetworkDiagnosticsSnapshot s;s.gateway_host=host;s.gateway_boot_id=boot;s.gateway_session=session;
    s.total_routes=static_cast<std::uint32_t>(d.routes.size());s.truncated=count<d.routes.size();
    s.routes.assign(d.routes.begin(),d.routes.begin()+count);output=std::move(s);return 0;
}
int EncodeNetworkDiagnosticsSnapshot(const NetworkDiagnosticsSnapshot& s,std::vector<std::uint8_t>& output) {
    if (!ValidHeader(s)) return -EINVAL;
    std::size_t count,size;const int result=Prefix(s.routes,count,size);if(result)return result;
    Writer w;w.bytes.reserve(size);w.U(MAGIC,4);w.U(NETWORK_DIAGNOSTICS_VERSION,2);
    w.U(count<s.total_routes?1:0,2);w.U(size,4);w.Host(s.gateway_host);w.Host(s.gateway_boot_id);
    w.U(s.gateway_session,8);w.U(s.total_routes,4);w.U(count,4);
    for(std::size_t i=0;i<count;++i) {
        const auto& r=s.routes[i];w.Endpoint(r.source);w.Endpoint(r.destination);
        w.U(r.source_session,8);w.U(r.destination_session,8);w.U(r.route_id,8);
        w.U(static_cast<unsigned>(r.state),1);w.U(r.sending?1:0,1);
        w.U(static_cast<unsigned>(r.reason),1);w.U(static_cast<unsigned>(r.proxy_recovery),1);
        w.U(static_cast<std::uint32_t>(r.error),4);
        for(auto member:COUNTERS)w.U(r.*member,8);
        w.U(r.name.size(),2);w.bytes.insert(w.bytes.end(),r.name.begin(),r.name.end());
    }
    output=std::move(w.bytes);return 0;
}
int DecodeNetworkDiagnosticsSnapshot(const std::uint8_t* data,std::size_t size,NetworkDiagnosticsSnapshot& output) {
    if(!data)return -EINVAL;
    if(size<HEADER || size>MAX_NETWORK_DIAGNOSTICS_PAYLOAD)return -EMSGSIZE;
    Reader r{data,size};if(r.U(4)!=MAGIC)return -EPROTO;
    if(r.U(2)!=NETWORK_DIAGNOSTICS_VERSION)return -EPROTONOSUPPORT;
    const auto flags=r.U(2);if(flags>1 || r.U(4)!=size)return -EPROTO;
    NetworkDiagnosticsSnapshot s;s.truncated=flags!=0;s.gateway_host=r.Host();s.gateway_boot_id=r.Host();
    s.gateway_session=r.U(8);s.total_routes=static_cast<std::uint32_t>(r.U(4));const auto count=r.U(4);
    if(count>s.total_routes || count>r.left/(ROUTE_FIXED+1))return -EPROTO;
    for(std::size_t i=0;i<count;++i) {
        TopicRouteInfo route;route.source=r.Endpoint();route.destination=r.Endpoint();
        route.source_session=r.U(8);route.destination_session=r.U(8);route.route_id=r.U(8);
        route.state=static_cast<TopicRouteState>(r.U(1));const auto sending=r.U(1);
        if(sending>1)return -EPROTO;
        route.sending=sending!=0;route.reason=static_cast<CompatibilityReason>(r.U(1));
        route.proxy_recovery=static_cast<ProxyRecoveryState>(r.U(1));
        const auto error=r.U(4);
        route.error=static_cast<int>(error<=INT32_MAX?static_cast<std::int64_t>(error):static_cast<std::int64_t>(error)-0x100000000LL);
        for(auto member:COUNTERS)route.*member=r.U(8);
        const auto length=r.U(2);
        if(!r.ok || !length || length>240 || length>r.left)return -EPROTO;
        route.name.assign(reinterpret_cast<const char*>(r.p),length);r.p+=length;r.left-=length;
        if(!ValidRoute(route))return -EPROTO;
        s.routes.push_back(std::move(route));
    }
    if(!r.ok || r.left || !ValidHeader(s))return -EPROTO;
    output=std::move(s);return 0;
}
}
