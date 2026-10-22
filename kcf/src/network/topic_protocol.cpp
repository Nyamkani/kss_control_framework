#include "kcf/network/topic_protocol.hpp"
#include <cerrno>
namespace kcf::network {
namespace {
struct Writer {
    std::vector<std::uint8_t> bytes;
    void U(std::uint64_t value,unsigned size){while(size)bytes.push_back(value>>(8*--size));}
    void Endpoint(const RemoteEndpointIdentity& e){for(auto b:e.runtime.host.bytes)U(b,1);for(auto b:e.runtime.boot_id.bytes)U(b,1);U(e.runtime.pid,4);U(e.runtime.process_start_ticks,8);U(e.registration_id,8);}
};
struct Reader {
    const std::uint8_t* p;std::size_t left;bool ok{true};
    std::uint64_t U(unsigned size){if(left<size){ok=false;return 0;}std::uint64_t value=0;while(size--){value=(value<<8)|*p++;--left;}return value;}
    RemoteEndpointIdentity Endpoint(){RemoteEndpointIdentity e;for(auto& b:e.runtime.host.bytes)b=U(1);for(auto& b:e.runtime.boot_id.bytes)b=U(1);auto pid=U(4);if(pid>INT32_MAX)ok=false;e.runtime.pid=pid;e.runtime.process_start_ticks=U(8);e.registration_id=U(8);return e;}
};
bool ValidPacket(const TopicPacket& p){return Valid(p.source)&&Valid(p.destination)&&p.source.runtime.host!=p.destination.runtime.host&&p.source_session&&p.destination_session&&p.route_id&&
    Valid(p.type)&&p.type.payload_size<=MAX_TOPIC_WIRE_PAYLOAD&&Text(p.name,240)&&p.name[0]=='/'&&
    ((p.kind==TopicMessage::INTEREST&&!p.sequence&&!p.sample_sequence&&p.payload.empty())||
     (p.kind==TopicMessage::DATA&&p.sequence&&p.sample_sequence&&p.payload.size()==p.type.payload_size));}
}
int EncodeTopicPacket(const TopicPacket& p,std::vector<std::uint8_t>& output){
    if(p.type.payload_size>MAX_TOPIC_WIRE_PAYLOAD)return -EMSGSIZE;
    if(!ValidPacket(p))return -EINVAL;
    Writer w;w.U(TOPIC_DATA_MAGIC,4);w.U(TOPIC_DATA_VERSION,2);w.U(static_cast<unsigned>(p.kind),2);
    w.U(TOPIC_DATA_HEADER+p.name.size()+p.payload.size(),2);w.U(0,2);w.U(p.name.size(),2);w.U(p.payload.size(),2);
    w.Endpoint(p.source);w.Endpoint(p.destination);w.U(p.source_session,8);w.U(p.destination_session,8);w.U(p.route_id,8);w.U(p.sequence,8);w.U(p.sample_sequence,8);
    w.U(p.type.type_id,8);w.U(p.type.schema_id,8);w.U(p.type.encoding_id,4);w.U(p.type.payload_size,4);
    w.bytes.insert(w.bytes.end(),p.name.begin(),p.name.end());w.bytes.insert(w.bytes.end(),p.payload.begin(),p.payload.end());output=std::move(w.bytes);return 0;
}
int DecodeTopicPacket(const std::uint8_t* data,std::size_t size,TopicPacket& output){
    if(!data||size<TOPIC_DATA_HEADER||size>MAX_TOPIC_DATAGRAM)return -EMSGSIZE;
    Reader r{data,size};if(r.U(4)!=TOPIC_DATA_MAGIC)return -EPROTO;if(r.U(2)!=TOPIC_DATA_VERSION)return -EPROTONOSUPPORT;
    TopicPacket p;p.kind=static_cast<TopicMessage>(r.U(2));if(r.U(2)!=size||r.U(2))return -EPROTO;
    auto names=r.U(2),payload=r.U(2);p.source=r.Endpoint();p.destination=r.Endpoint();p.source_session=r.U(8);p.destination_session=r.U(8);p.route_id=r.U(8);p.sequence=r.U(8);p.sample_sequence=r.U(8);
    p.type.type_id=r.U(8);p.type.schema_id=r.U(8);p.type.encoding_id=r.U(4);p.type.payload_size=r.U(4);
    if(!r.ok||names>240||payload>MAX_TOPIC_WIRE_PAYLOAD||names+payload!=r.left)return -EPROTO;
    p.name.assign(reinterpret_cast<const char*>(r.p),names);r.p+=names;p.payload.assign(r.p,r.p+payload);
    if(!ValidPacket(p))return -EPROTO;
    output=std::move(p);return 0;
}
}
