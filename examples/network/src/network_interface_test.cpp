#include "kcf/network/network_interface.hpp"
#include <cassert>
#include <cerrno>
#include <iostream>
#include <dirent.h>
namespace n=kcf::network;
int main() {
    n::NetworkInterfaceInfo a{"test-a","192.0.2.10",7,true,true,true,false,false};
    auto b=a;b.name="test-b";b.index=8;b.ipv4_address="192.0.2.11";
    n::NetworkInterfaceSelection out;
    const auto fail=[&](std::vector<n::NetworkInterfaceInfo> rows,n::NetworkInterfaceOptions options,int error) {
        out={"unchanged","sentinel",999,false};
        assert(n::SelectNetworkInterface(rows,options,out)==error);
        assert(out.name=="unchanged" && out.ipv4_address=="sentinel" && out.index==999 && !out.automatic);
    };
    assert(n::SelectNetworkInterface({a},{},out)==0 && out.automatic && out.index==7 && out.ipv4_address==a.ipv4_address);
    fail({a,b},{},-ENOTUNIQ);fail({}, {},-ENETDOWN);
    assert(n::SelectNetworkInterface({a,a},{},out)==0);
    assert(n::SelectNetworkInterface({a,b},{a.name,"",false},out)==0 && !out.automatic && out.name==a.name);
    assert(n::SelectNetworkInterface({a,b},{"",b.ipv4_address,false},out)==0 && !out.automatic && out.index==b.index);
    fail({a},{"absent","",false},-ENODEV);
    fail({a},{"","192.0.2.99",false},-EADDRNOTAVAIL);
    fail({a},{a.name,a.ipv4_address,false},-EINVAL);
    for(auto ip:{"broken","0.0.0.0","239.1.2.3","255.255.255.255","::1"})fail({a},{"",ip,false},-EINVAL);
    fail({a},{std::string("bad\0name",8),"",false},-EINVAL);
    auto down=a;down.up=false;fail({down},{},-ENETDOWN);fail({down},{a.name,"",false},-ENETDOWN);
    down=a;down.running=false;fail({down},{},-ENETDOWN);
    down=a;down.multicast=false;fail({down},{},-ENETDOWN);
    auto loop=a;loop.loopback=true;loop.ipv4_address="127.0.0.1";loop.multicast=false;
    fail({loop},{},-ENETDOWN);fail({loop},{"","127.0.0.1",false},-ENETDOWN);
    assert(n::SelectNetworkInterface({loop},{"","127.0.0.1",true},out)==0 && !out.automatic);
    fail({loop},{"","",true},-ENETDOWN);
    auto tunnel=a;tunnel.point_to_point=true;fail({tunnel},{},-ENETDOWN);
    assert(n::SelectNetworkInterface({tunnel},{a.name,"",false},out)==0);
    auto alias=a;alias.ipv4_address="192.0.2.20";
    fail({a,alias},{a.name,"",false},-ENOTUNIQ);
    assert(n::SelectNetworkInterface({a,alias},{"",alias.ipv4_address,false},out)==0);
    b.ipv4_address=a.ipv4_address;fail({a,b},{"",a.ipv4_address,false},-ENOTUNIQ);
    auto invalid=a;invalid.index=0;fail({invalid},{},-ENETDOWN);
    invalid=a;invalid.ipv4_address="bad";fail({invalid},{},-ENETDOWN);
    assert(n::SelectNetworkInterface({loop,down,a},{},out)==0 && out.name==a.name);
    // Real enumeration smoke only: no assumptions about NIC count/name/reachability.
    const auto fds=[] {
        DIR* dir=opendir("/proc/self/fd");assert(dir);unsigned count=0;
        while(readdir(dir))++count;
        closedir(dir);return count;
    };
    const auto baseline=fds();
    std::vector<n::NetworkInterfaceInfo> rows;
    for(int i=0;i<100;++i)assert(n::EnumerateNetworkInterfaces(rows)==0);
    assert(fds()==baseline);
    for(const auto& nic:rows)assert(nic.index && !nic.name.empty() && !nic.ipv4_address.empty());
    out={"sentinel","sentinel",999,false};
    assert(n::ResolveNetworkInterface({"","not-an-ip",false},out)==-EINVAL && out.index==999);
    std::cout<<"Interface resolver synthetic selection/filter/override/ambiguity/output preservation PASS; enumeration PASS\n";
}
