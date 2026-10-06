#pragma once
#include "kcf/introspection/type_descriptor.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pubsub
{
inline constexpr char TOPIC[]="/example/chatter";
struct PubSubMessage
{
    std::uint64_t sequence{0};
    std::uint64_t timestamp_us{0};
    std::int32_t count{0};
    float value{0.0f};
    bool active{false};
};
static_assert(std::is_trivially_copyable_v<PubSubMessage>);
static_assert(std::is_standard_layout_v<PubSubMessage>);
}
namespace kcf
{
template<> struct TypeDescriptorTraits<pubsub::PubSubMessage>
{
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept
    {
        using T=pubsub::PubSubMessage;
        auto d=MakeTypeDescriptor<T>("kcf.example.PubSubMessage");
        d.protocol_version=1;
        d.field_count=5;
        d.fields[0]=MakeField<T,std::uint64_t>("sequence",offsetof(T,sequence));
        d.fields[1]=MakeField<T,std::uint64_t>("timestamp_us",offsetof(T,timestamp_us));
        d.fields[2]=MakeField<T,std::int32_t>("count",offsetof(T,count));
        d.fields[3]=MakeField<T,float>("value",offsetof(T,value));
        d.fields[4]=MakeField<T,bool>("active",offsetof(T,active));
        return d;
    }
};
}
