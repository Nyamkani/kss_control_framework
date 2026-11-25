#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <optional>
#include <string>
namespace kcf::network::detail {
// Explicit local payload contract, never infer a clock from a field name.
inline std::optional<std::size_t> TopicTimestampOffset(const TypeDescriptor& d,const std::string& name) {
    if(d.field_count>MAX_TYPE_FIELDS)return {};
    std::optional<std::size_t> result;
    for(std::uint32_t i=0;i<d.field_count;++i){const auto& f=d.fields[i];
        const auto end=std::char_traits<char>::find(f.name,sizeof(f.name),'\0');
        if(!end)return {};
        if(std::string(f.name,end)!=name)continue;
        if(result||f.kind!=FieldValueKind::UINT64||f.element_size!=8||f.array_count!=1||
           f.offset>d.payload_size||d.payload_size-f.offset<8)return {};
        result=f.offset;
    }
    return result;
}
// Window: [scheduled-period, scheduled+tolerance]. The deadline is fixed,
// never extended by retry. now/stamp/window are local CLOCK_MONOTONIC ns.
inline bool TopicTimestampEligible(std::uint64_t stamp,std::uint64_t sequence,
        std::uint64_t last_sequence,std::uint64_t last_stamp,
        std::uint64_t earliest,std::uint64_t expiry,std::uint64_t now) {
    return stamp!=0&&sequence>last_sequence&&stamp>=last_stamp&&
           stamp>=earliest&&stamp<=now&&now<=expiry;
}
}
