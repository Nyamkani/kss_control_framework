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
