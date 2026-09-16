#pragma once
#include "kcf/system/system_status.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace kcf::detail {
inline constexpr char SYSTEM_STATUS_SCOPE_ENV[] = "KCF_SYSTEM_STATUS_SCOPE";
inline std::string SystemStatusScope(std::int32_t pid, std::uint64_t ticks) {
    return pid > 0 && ticks ? std::to_string(pid)+"_"+std::to_string(ticks) : std::string{};
}
inline int CurrentSystemStatusScope(std::string& scope) {
    std::uint64_t ticks=0;
    if(!ReadProcessIdentity(getpid(),ticks))return -ESRCH;
    scope=SystemStatusScope(getpid(),ticks);return 0;
}
inline std::string InheritedSystemStatusScope() {
    const char* value=std::getenv(SYSTEM_STATUS_SCOPE_ENV);
    return value?value:"";
}
inline int SystemStatusName(const std::string& scope,std::string& name) {
    if(scope.empty()){name=SYSTEM_STATUS_STORAGE;return 0;}
    const auto separator=scope.find('_');
    if(separator==std::string::npos)return -EINVAL;
    std::int32_t pid=0;std::uint64_t ticks=0;
    const char* begin=scope.data();const char* end=begin+scope.size();
    const auto p=std::from_chars(begin,begin+separator,pid);
    const auto t=std::from_chars(begin+separator+1,end,ticks);
    if(p.ec!=std::errc{} || t.ec!=std::errc{} || p.ptr!=begin+separator || t.ptr!=end ||
       pid<=0 || !ticks || scope!=SystemStatusScope(pid,ticks))return -EINVAL;
    name=std::string(SYSTEM_STATUS_STORAGE)+"_"+scope;return 0;
}
}
