#include "kcf/network/parameter_codec.hpp"
#include <cerrno>
#include <cstring>
#include <algorithm>
namespace kcf::network {
int ParameterWireType(const TypeDescriptor& d,TypeIdentity& out) {
    if(!ValidateTypeDescriptor(d) || !d.field_count || d.payload_size>MAX_PARAMETER_WIRE)return -EINVAL;
    std::uint64_t hash=14695981039346656037ull;std::size_t size=0;
    auto byte=[&](std::uint8_t b){hash^=b;hash*=1099511628211ull;};
    for(std::uint32_t i=0;i<d.field_count;++i) {
        const auto& f=d.fields[i];for(const char* p=f.name;*p;++p)byte(*p);byte(0);byte(static_cast<std::uint8_t>(f.kind));
        for(int j=3;j>=0;--j)byte(static_cast<std::uint8_t>(f.array_count>>(8*j)));
        size+=static_cast<std::size_t>(f.element_size)*f.array_count;
    }
    if(!hash || !size || size>MAX_PARAMETER_WIRE)return -EMSGSIZE;
    out={d.type_id,hash,1,static_cast<std::uint32_t>(size)};return 0;
}
int DescriptorType(const NetworkEndpointMetadata& m,DescriptorRole role,TypeIdentity& out) {
    if(role==DescriptorRole::VALUE&&(m.kind==EndpointKind::PARAMETER||m.kind==EndpointKind::TOPIC))out=m.value_type;
    else if(role==DescriptorRole::REQUEST&&(m.kind==EndpointKind::SERVICE||m.kind==EndpointKind::ACTION))out=m.request_type;
    else if(role==DescriptorRole::RESPONSE&&(m.kind==EndpointKind::SERVICE||m.kind==EndpointKind::ACTION))out=m.response_type;
    else if(role==DescriptorRole::FEEDBACK&&m.kind==EndpointKind::ACTION)out=m.feedback_type;
    else return -EINVAL;
    return 0;
}
int EncodeTypeDescriptor(const TypeDescriptor& d,std::vector<std::uint8_t>& output) {
    TypeIdentity type;int code=ParameterWireType(d,type);if(code)return code;
    std::vector<std::uint8_t> bytes;
    auto u=[&](std::uint32_t value,unsigned width){for(unsigned i=width;i;--i)bytes.push_back(static_cast<std::uint8_t>(value>>((i-1)*8)));};
    auto text=[&](const char* name){auto size=std::strlen(name);u(size,2);bytes.insert(bytes.end(),name,name+size);};
    u(1,2);text(d.type_name);u(d.field_count,2);
    for(std::uint32_t i=0;i<d.field_count;++i){const auto& f=d.fields[i];text(f.name);u(static_cast<unsigned>(f.kind),1);u(f.array_count,4);}
    if(bytes.size()>MAX_PARAMETER_WIRE)return -EMSGSIZE;
    output=std::move(bytes);return 0;
}
int DecodeTypeDescriptor(const std::vector<std::uint8_t>& bytes,const TypeIdentity& expected,TypeDescriptor& output) {
    if(bytes.size()>MAX_PARAMETER_WIRE)return -EMSGSIZE;
    if(bytes.empty())return -EPROTO;
    std::size_t cursor=0;bool good=true;
    auto u=[&](unsigned width){std::uint32_t value=0;if(width>bytes.size()-cursor){good=false;return value;}while(width--)value=(value<<8)|bytes[cursor++];return value;};
    auto text=[&](char* name,std::size_t capacity){auto length=u(2);if(!good||!length||length>=capacity||length>bytes.size()-cursor){good=false;return;}
        for(unsigned i=0;i<length;++i){auto c=bytes[cursor++];if(!c)good=false;name[i]=static_cast<char>(c);} };
    if(u(2)!=1)return -EPROTONOSUPPORT;
    TypeDescriptor d{};text(d.type_name,sizeof(d.type_name));d.type_id=StableTypeId(d.type_name);d.payload_alignment=1;d.field_count=u(2);
    if(!good||!d.field_count||d.field_count>MAX_TYPE_FIELDS)return -EPROTO;
    for(std::uint32_t i=0;i<d.field_count;++i){auto& f=d.fields[i];text(f.name,sizeof(f.name));f.kind=static_cast<FieldValueKind>(u(1));f.array_count=u(4);f.element_size=PrimitiveSize(f.kind);
        auto size=std::uint64_t(f.element_size)*f.array_count;
        if(!good||!f.element_size||!f.array_count||size>MAX_PARAMETER_WIRE-d.payload_size)return -EPROTO;
        f.offset=d.payload_size;d.payload_size+=static_cast<std::uint32_t>(size);
    }
    if(!good||cursor!=bytes.size())return -EPROTO;
    TypeIdentity actual;int code=ParameterWireType(d,actual);if(code)return -EPROTO;
    if(!(actual==expected))return -EPROTOTYPE;
    output=d;return 0;
}
int EncodeParameterValue(const TypeDescriptor& d,const DynamicPayload& value,std::vector<std::uint8_t>& output) {
    TypeIdentity type;int code=ParameterWireType(d,type);if(code)return code;
    if(value.type_id!=d.type_id || value.bytes.size()!=d.payload_size)return -EPROTOTYPE;
    std::vector<std::uint8_t> bytes;bytes.reserve(type.payload_size);
    const std::uint16_t endian=1;const bool little=*reinterpret_cast<const unsigned char*>(&endian)==1;
    for(std::uint32_t i=0;i<d.field_count;++i) {
        const auto& f=d.fields[i];
        for(std::uint32_t n=0;n<f.array_count;++n) {
            const auto* p=reinterpret_cast<const std::uint8_t*>(value.bytes.data())+f.offset+n*f.element_size;
            if(f.kind==FieldValueKind::BOOL && p[0]>1)return -EPROTO;
            for(std::uint32_t j=0;j<f.element_size;++j)bytes.push_back(p[little?f.element_size-1-j:j]);
        }
    }
    output=std::move(bytes);return 0;
}
int DecodeParameterValue(const TypeDescriptor& d,const std::vector<std::uint8_t>& input,DynamicPayload& output) {
    TypeIdentity type;int code=ParameterWireType(d,type);if(code)return code;
    if(input.size()!=type.payload_size)return -EMSGSIZE;
    DynamicPayload value;value.type_id=d.type_id;value.bytes.resize(d.payload_size);std::size_t cursor=0;
    const std::uint16_t endian=1;const bool little=*reinterpret_cast<const unsigned char*>(&endian)==1;
    for(std::uint32_t i=0;i<d.field_count;++i) {
        const auto& f=d.fields[i];
        for(std::uint32_t n=0;n<f.array_count;++n) {
            auto* p=reinterpret_cast<std::uint8_t*>(value.bytes.data())+f.offset+n*f.element_size;
            if(f.kind==FieldValueKind::BOOL && input[cursor]>1)return -EPROTO;
            for(std::uint32_t j=0;j<f.element_size;++j)p[little?f.element_size-1-j:j]=input[cursor++];
        }
    }
    output=std::move(value);return 0;
}
}
