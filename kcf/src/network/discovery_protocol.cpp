#include "kcf/network/discovery_protocol.hpp"
#include "kcf/network/validation.hpp"
#include <cerrno>
#include <limits>
#include <new>

namespace kcf::network
{
namespace
{
struct Writer
{
    std::vector<std::uint8_t> bytes;
    void U(std::uint64_t value, unsigned width) {
        for (unsigned i = width; i; --i) bytes.push_back(static_cast<std::uint8_t>(value >> ((i-1)*8)));
    }
    void Host(const HostIdentity& id) { bytes.insert(bytes.end(), id.bytes.begin(), id.bytes.end()); }
    void Runtime(const RemoteRuntimeIdentity& r) { Host(r.host); Host(r.boot_id); U(r.pid,4); U(r.process_start_ticks,8); }
    void Type(const TypeIdentity& t) { U(t.type_id,8); U(t.schema_id,8); U(t.encoding_id,4); U(t.payload_size,4); }
    void String(const std::string& s) { U(s.size(),2); bytes.insert(bytes.end(),s.begin(),s.end()); }
};
struct Reader
{
    const std::uint8_t* p; std::size_t left; bool ok{true};
    std::uint64_t U(unsigned width) {
        if (left < width) { ok=false; return 0; }
        std::uint64_t value=0; for(unsigned i=0;i<width;++i) value=(value<<8)|*p++;
        left-=width; return value;
    }
    HostIdentity Host() { HostIdentity id; for(auto& b:id.bytes) b=static_cast<std::uint8_t>(U(1)); return id; }
    RemoteRuntimeIdentity Runtime() {
        RemoteRuntimeIdentity r; r.host=Host(); r.boot_id=Host(); const auto pid=U(4);
        if(pid>static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) ok=false;
        r.pid=static_cast<std::int32_t>(pid); r.process_start_ticks=U(8); return r;
    }
    TypeIdentity Type() { TypeIdentity t; t.type_id=U(8);t.schema_id=U(8);t.encoding_id=U(4);t.payload_size=U(4);return t; }
    std::string String(std::size_t maximum) {
        const auto length=U(2); if(!ok || length>maximum || length>left) { ok=false; return {}; }
        std::string s(reinterpret_cast<const char*>(p),length);p+=length;left-=length;return s;
    }
};
bool ValidPacket(const DiscoveryPacket& p)
{
    if(!Valid(p.host)||!Valid(p.boot_id)||!p.session_id||!p.revision) return false;
    switch(p.kind) {
    case DiscoveryMessage::ANNOUNCE:
        return Valid(p.profile) && (p.profile.capabilities & CapabilityBit(Capability::DISCOVERY)) &&
            p.sequence && !p.request_id && !p.offset && p.chunk.empty() &&
            p.metadata_size>=2 && p.metadata_size<=DISCOVERY_MAX_METADATA && p.endpoint_count<=DISCOVERY_MAX_ENDPOINTS;
    case DiscoveryMessage::METADATA_REQUEST:
        return p.request_id && !p.sequence && !p.metadata_size && !p.endpoint_count && p.chunk.empty() &&
            p.offset<DISCOVERY_MAX_METADATA && p.offset%DISCOVERY_CHUNK_SIZE==0;
    case DiscoveryMessage::METADATA_RESPONSE:
        return p.request_id && !p.sequence && !p.endpoint_count && p.metadata_size>=2 && p.metadata_size<=DISCOVERY_MAX_METADATA &&
            p.offset<p.metadata_size && p.offset%DISCOVERY_CHUNK_SIZE==0 &&
            p.chunk.size()==std::min<std::size_t>(DISCOVERY_CHUNK_SIZE,p.metadata_size-p.offset);
    }
    return false;
}
}
int EncodeDiscoveryPacket(const DiscoveryPacket& p, std::vector<std::uint8_t>& output)
{
    if(!ValidPacket(p)) return -EINVAL;
    try {
        Writer w; w.U(DISCOVERY_MAGIC,4);w.U(DISCOVERY_VERSION,2);w.U(static_cast<unsigned>(p.kind),2);
        const auto size=DISCOVERY_HEADER_SIZE+(p.kind==DiscoveryMessage::ANNOUNCE?44:(p.kind==DiscoveryMessage::METADATA_REQUEST?20:24))+p.chunk.size();
        w.U(size,4);w.U(0,4);w.Host(p.host);w.Host(p.boot_id);w.U(p.session_id,8);
        if(p.kind==DiscoveryMessage::ANNOUNCE) {
            w.U(p.sequence,8);w.U(p.revision,8);w.U(p.metadata_size,4);w.U(p.endpoint_count,2);w.U(0,2);
            w.U(p.profile.framework.major,2);w.U(p.profile.framework.minor,2);w.U(p.profile.framework.patch,2);
            w.U(p.profile.network.major,2);w.U(p.profile.network.minor,2);w.U(0,2);w.U(p.profile.capabilities,8);
        } else {
            w.U(p.request_id,8);w.U(p.revision,8);w.U(p.offset,4);
            if(p.kind==DiscoveryMessage::METADATA_RESPONSE) {w.U(p.metadata_size,4);w.bytes.insert(w.bytes.end(),p.chunk.begin(),p.chunk.end());}
        }
        output=std::move(w.bytes);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
int DecodeDiscoveryPacket(const std::uint8_t* data, std::size_t size, DiscoveryPacket& output)
{
    if(!data) return -EINVAL;
    if(size<DISCOVERY_HEADER_SIZE || size>DISCOVERY_MAX_DATAGRAM) return -EMSGSIZE;
    try {
        Reader r{data,size}; if(r.U(4)!=DISCOVERY_MAGIC) return -EPROTO;
        if(r.U(2)!=DISCOVERY_VERSION) return -EPROTONOSUPPORT;
        DiscoveryPacket p;p.kind=static_cast<DiscoveryMessage>(r.U(2));
        if(r.U(4)!=size) return -EMSGSIZE;
        if(r.U(4)!=0) return -EPROTO;
        p.host=r.Host();p.boot_id=r.Host();p.session_id=r.U(8);
        if(p.kind==DiscoveryMessage::ANNOUNCE) {
            p.sequence=r.U(8);p.revision=r.U(8);p.metadata_size=r.U(4);p.endpoint_count=r.U(2);
            if(r.U(2)) return -EPROTO;
            p.profile.framework.major=r.U(2);p.profile.framework.minor=r.U(2);p.profile.framework.patch=r.U(2);
            p.profile.network.major=r.U(2);p.profile.network.minor=r.U(2);
            if(r.U(2)) return -EPROTO;
            p.profile.capabilities=r.U(8);
        } else if(p.kind==DiscoveryMessage::METADATA_REQUEST || p.kind==DiscoveryMessage::METADATA_RESPONSE) {
            p.request_id=r.U(8);p.revision=r.U(8);p.offset=r.U(4);
            if(p.kind==DiscoveryMessage::METADATA_RESPONSE) {
                p.metadata_size=r.U(4);if(r.left>DISCOVERY_CHUNK_SIZE) return -EMSGSIZE;
                if(r.ok) {p.chunk.assign(r.p,r.p+r.left);r.left=0;}
            }
        } else return -EPROTO;
        if(!r.ok || r.left || !ValidPacket(p)) return -EPROTO;
        output=std::move(p);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
bool ValidDiscoveryMetadata(const std::vector<NetworkEndpointMetadata>& endpoints, const HostIdentity& host, const HostIdentity& boot)
{
    if(!Valid(host)||!Valid(boot)||endpoints.size()>DISCOVERY_MAX_ENDPOINTS) return false;
    for(std::size_t i=0;i<endpoints.size();++i) {
        if(!Valid(endpoints[i]) || endpoints[i].identity.runtime.host!=host || endpoints[i].identity.runtime.boot_id!=boot) return false;
        for(std::size_t j=0;j<i;++j) if(endpoints[i].identity==endpoints[j].identity) return false;
    }
    return true;
}
int EncodeDiscoveryMetadata(const std::vector<NetworkEndpointMetadata>& endpoints, std::vector<std::uint8_t>& output)
{
    if(endpoints.size()>DISCOVERY_MAX_ENDPOINTS) return -E2BIG;
    try {
        Writer w;w.U(endpoints.size(),2);
        for(const auto& m:endpoints) {
            if(!Valid(m)) return -EINVAL;
            w.U(m.protocol_version,2);w.Runtime(m.identity.runtime);w.U(m.identity.registration_id,8);w.Runtime(m.application);
            w.U(static_cast<unsigned>(m.kind),1);w.U(static_cast<unsigned>(m.role),1);w.U(static_cast<unsigned>(m.scope),1);w.U(0,1);
            w.Type(m.value_type);w.Type(m.request_type);w.Type(m.response_type);w.Type(m.feedback_type);
            w.String(m.host_name);w.String(m.application_name);w.String(m.element_name);w.String(m.name);
            w.U(m.groups.size(),2);for(const auto& group:m.groups) w.String(group);
            if(w.bytes.size()>DISCOVERY_MAX_METADATA) return -E2BIG;
        }
        output=std::move(w.bytes);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
int DecodeDiscoveryMetadata(const std::uint8_t* data, std::size_t size, std::vector<NetworkEndpointMetadata>& output)
{
    if(!data) return -EINVAL;
    if(size<2 || size>DISCOVERY_MAX_METADATA) return -EMSGSIZE;
    try {
        Reader r{data,size};const auto count=r.U(2);if(count>DISCOVERY_MAX_ENDPOINTS) return -E2BIG;
        std::vector<NetworkEndpointMetadata> decoded;
        for(std::size_t i=0;i<count;++i) {
            NetworkEndpointMetadata m;m.protocol_version=r.U(2);m.identity.runtime=r.Runtime();m.identity.registration_id=r.U(8);m.application=r.Runtime();
            m.kind=static_cast<EndpointKind>(r.U(1));m.role=static_cast<EndpointRole>(r.U(1));m.scope=static_cast<NetworkScope>(r.U(1));
            if(r.U(1)!=0) return -EPROTO;
            m.value_type=r.Type();m.request_type=r.Type();m.response_type=r.Type();m.feedback_type=r.Type();
            m.host_name=r.String(128);m.application_name=r.String(128);m.element_name=r.String(128);m.name=r.String(240);
            const auto groups=r.U(2);if(groups>64) return -EPROTO;
            for(std::size_t j=0;j<groups && r.ok;++j) m.groups.push_back(r.String(128));
            if(!r.ok || !Valid(m)) return -EPROTO;
            decoded.push_back(std::move(m));
        }
        if(!r.ok || r.left) return -EPROTO;
        output=std::move(decoded);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}
} // namespace kcf::network
