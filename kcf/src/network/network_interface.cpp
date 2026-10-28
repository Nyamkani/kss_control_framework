#include "kcf/network/network_interface.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <new>
#include <set>
#include <tuple>

namespace kcf::network {
namespace {
bool Name(const std::string& name) {
    if(name.empty() || name.size()>=IFNAMSIZ)return false;
    for(unsigned char c:name)if(c==0 || c=='/' || c==':' || c<=32 || c==127)return false;
    return true;
}
bool Address(const std::string& text, bool& loopback) {
    in_addr address{};
    if(text.find('\0')!=std::string::npos || inet_pton(AF_INET,text.c_str(),&address)!=1)return false;
    const auto value=ntohl(address.s_addr);
    loopback=(value>>24)==127;
    return (value>>24)!=0 && (value>>28)<14; // exclude unspecified, multicast, reserved/broadcast
}
int Options(const NetworkInterfaceOptions& options) {
    if(!options.interface_name.empty() && !options.ipv4_address.empty())return -EINVAL;
    if(!options.interface_name.empty() && !Name(options.interface_name))return -EINVAL;
    bool loopback=false;
    if(!options.ipv4_address.empty() && !Address(options.ipv4_address,loopback))return -EINVAL;
    return 0;
}
}
int EnumerateNetworkInterfaces(std::vector<NetworkInterfaceInfo>& output) try {
    ifaddrs* raw=nullptr;
    if(getifaddrs(&raw))return -errno;
    std::unique_ptr<ifaddrs,decltype(&freeifaddrs)> list(raw,freeifaddrs);
    std::vector<NetworkInterfaceInfo> found;
    for(auto* p=raw;p;p=p->ifa_next) {
        if(!p->ifa_addr || !p->ifa_name || p->ifa_addr->sa_family!=AF_INET)continue;
        char address[INET_ADDRSTRLEN]{};
        if(!inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr,address,sizeof(address)))return -errno;
        errno=0;const auto index=if_nametoindex(p->ifa_name);
        if(!index)return errno?-errno:-ENODEV; // interface disappeared; don't silently choose another
        const auto flags=p->ifa_flags;
        found.push_back({p->ifa_name,address,index,bool(flags&IFF_UP),bool(flags&IFF_RUNNING),
                         bool(flags&IFF_MULTICAST),bool(flags&IFF_LOOPBACK),bool(flags&IFF_POINTOPOINT)});
    }
    output=std::move(found);return 0;
} catch(const std::bad_alloc&) {return -ENOMEM;}
int SelectNetworkInterface(const std::vector<NetworkInterfaceInfo>& interfaces,
                           const NetworkInterfaceOptions& options,NetworkInterfaceSelection& output) try {
    const int error=Options(options);if(error)return error;
    const bool automatic=options.interface_name.empty() && options.ipv4_address.empty();
    bool matched=false;
    std::set<std::tuple<std::string,unsigned int,std::string>> seen;
    NetworkInterfaceSelection selected;std::size_t count=0;
    for(const auto& nic:interfaces) {
        if(!options.interface_name.empty() && nic.name!=options.interface_name)continue;
        if(!options.ipv4_address.empty() && nic.ipv4_address!=options.ipv4_address)continue;
        matched=true;bool loopback=false;
        if(!Name(nic.name) || !nic.index || !Address(nic.ipv4_address,loopback))continue;
        loopback=loopback || nic.loopback;
        if(!nic.up || !nic.running)continue;
        if(loopback && (automatic || !options.allow_loopback))continue;
        if(!nic.multicast && !(loopback && !automatic && options.allow_loopback))continue;
        if(automatic && nic.point_to_point)continue;
        if(!seen.emplace(nic.name,nic.index,nic.ipv4_address).second)continue;
        selected={nic.name,nic.ipv4_address,nic.index,automatic};++count;
    }
    if(!matched && !automatic)return options.interface_name.empty()?-EADDRNOTAVAIL:-ENODEV;
    if(!count)return -ENETDOWN;
    if(count!=1)return -ENOTUNIQ;
    output=std::move(selected);return 0;
} catch(const std::bad_alloc&) {return -ENOMEM;}
int ResolveNetworkInterface(const NetworkInterfaceOptions& options,NetworkInterfaceSelection& output) {
    const int error=Options(options);if(error)return error;
    std::vector<NetworkInterfaceInfo> interfaces;
    const int result=EnumerateNetworkInterfaces(interfaces);
    return result?result:SelectNetworkInterface(interfaces,options,output);
}
}
