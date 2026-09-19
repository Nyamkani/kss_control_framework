#pragma once
#include "kcf/introspection/type_descriptor.hpp"
struct TestData { std::uint64_t sequence; float x; std::int32_t count; float values[4]; };
struct UnknownData { int value; };
struct BadData { int value; };
struct ConflictData { std::uint64_t sequence; float x; std::int32_t count; float values[4]; };
struct Header { std::uint64_t sequence, timestamp_us; };
struct NestedData { Header header; float x; };
struct ExtraData { double value; };
struct CapacityData { int value; };
namespace kcf {
template<> struct TypeDescriptorTraits<TestData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<TestData>("kcf.test.TestData.v1");d.field_count=4;
        d.fields[0]=MakeField<TestData,std::uint64_t>("sequence",offsetof(TestData,sequence));
        d.fields[1]=MakeField<TestData,float>("x",offsetof(TestData,x));
        d.fields[2]=MakeField<TestData,std::int32_t>("count",offsetof(TestData,count));
        d.fields[3]=MakeField<TestData,float[4]>("values",offsetof(TestData,values));return d;
    }
};
template<> struct TypeDescriptorTraits<BadData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<BadData>("kcf.test.BadData.v1");d.field_count=1;
        d.fields[0]=MakeField<BadData,int>("value",sizeof(BadData));return d;
    }
};
template<> struct TypeDescriptorTraits<ConflictData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=TypeDescriptorTraits<TestData>::Get();
        d.fields[1]=MakeField<ConflictData,float>("different_x",offsetof(ConflictData,x));return d;
    }
};
template<> struct TypeDescriptorTraits<NestedData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<NestedData>("kcf.test.NestedData.v1");d.field_count=3;
        d.fields[0]=MakeField<NestedData,std::uint64_t>("header.sequence",offsetof(NestedData,header)+offsetof(Header,sequence));
        d.fields[1]=MakeField<NestedData,std::uint64_t>("header.timestamp_us",offsetof(NestedData,header)+offsetof(Header,timestamp_us));
        d.fields[2]=MakeField<NestedData,float>("x",offsetof(NestedData,x));return d;
    }
};
template<> struct TypeDescriptorTraits<CapacityData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<CapacityData>("kcf.test.CapacityData.v1");d.field_count=1;
        d.fields[0]=MakeField<CapacityData,int>("value",offsetof(CapacityData,value));return d;
    }
};
template<> struct TypeDescriptorTraits<ExtraData> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<ExtraData>("kcf.test.ExtraData.v1");d.field_count=1;
        d.fields[0]=MakeField<ExtraData,double>("value",offsetof(ExtraData,value));return d;
    }
};
}
