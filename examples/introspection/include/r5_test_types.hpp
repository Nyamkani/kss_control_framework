#pragma once
#include "kcf/introspection/type_descriptor.hpp"
struct R5Request {std::int32_t x,y;};
struct R5Response {std::int32_t x,y;};
struct R5WrongRequest {std::int32_t x,y;};
struct R5WrongResponse {std::int32_t x,y;};
struct R5Legacy {std::int32_t x,y;};
struct R5Conflict {std::int32_t x,y;};
namespace kcf {
template<class T> TypeDescriptor R5Descriptor(const char* name) {
    auto d=MakeTypeDescriptor<T>(name);d.field_count=2;
    d.fields[0]=MakeField<T,std::int32_t>("x",offsetof(T,x));
    d.fields[1]=MakeField<T,std::int32_t>("y",offsetof(T,y));return d;
}
template<> struct TypeDescriptorTraits<R5Request>{static constexpr bool defined=true;static TypeDescriptor Get() noexcept{return R5Descriptor<R5Request>("kcf.r5.Request.v1");}};
template<> struct TypeDescriptorTraits<R5Response>{static constexpr bool defined=true;static TypeDescriptor Get() noexcept{return R5Descriptor<R5Response>("kcf.r5.Response.v1");}};
template<> struct TypeDescriptorTraits<R5WrongRequest>{static constexpr bool defined=true;static TypeDescriptor Get() noexcept{return R5Descriptor<R5WrongRequest>("kcf.r5.WrongRequest.v1");}};
template<> struct TypeDescriptorTraits<R5WrongResponse>{static constexpr bool defined=true;static TypeDescriptor Get() noexcept{return R5Descriptor<R5WrongResponse>("kcf.r5.WrongResponse.v1");}};
template<> struct TypeDescriptorTraits<R5Conflict>{static constexpr bool defined=true;static TypeDescriptor Get() noexcept{
    auto d=R5Descriptor<R5Conflict>("kcf.r5.Request.v1");d.fields[0].name[0]='z';return d;}};
}
