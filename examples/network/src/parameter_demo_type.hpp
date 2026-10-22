#pragma once
#include "kcf/introspection/type_descriptor.hpp"
struct Value { std::int32_t count; bool enabled; double gain; };
namespace kcf {template<> struct TypeDescriptorTraits<Value> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() {auto d=MakeTypeDescriptor<Value>("kcf.network.parameter.v1");d.field_count=3;
        d.fields[0]=MakeField<Value,std::int32_t>("count",offsetof(Value,count));
        d.fields[1]=MakeField<Value,bool>("enabled",offsetof(Value,enabled));
        d.fields[2]=MakeField<Value,double>("gain",offsetof(Value,gain));return d;}
};}
