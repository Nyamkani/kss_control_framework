#pragma once
#include <string>
#include <vector>

namespace kcf::network {
struct NetworkInterfaceInfo {
    std::string name;
    std::string ipv4_address;
    unsigned int index{0};
    bool up{false}, running{false}, multicast{false}, loopback{false}, point_to_point{false};
};
struct NetworkInterfaceOptions {
    // Empty = automatic. Set exactly one override; no implicit fallback.
    std::string interface_name;
    std::string ipv4_address;
    // Test-only loopback selection requires an explicit override. Linux loopback
    // often lacks IFF_MULTICAST, so that flag is waived only for this opt-in.
    bool allow_loopback{false};
};
struct NetworkInterfaceSelection {
    std::string name;
    std::string ipv4_address; // directly usable as DiscoveryConfig::interface_address
    unsigned int index{0};
    bool automatic{true};
};
// getifaddrs IPv4 snapshot, including down interfaces for override diagnostics.
// No sockets retained. Interfaces without IPv4 are not included.
int EnumerateNetworkInterfaces(std::vector<NetworkInterfaceInfo>& output);
// Pure selection over a supplied snapshot, suitable for deterministic tests.
// Automatic: UP+RUNNING+MULTICAST, unicast IPv4, non-loopback/non-point-to-point.
// Multiple addresses on one NIC are also ambiguous; use an IPv4 override.
// Duplicate identical name/index/address rows are counted once.
// Explicit overrides still require UP/RUNNING and multicast (loopback exception
// above). Point-to-point may be explicitly selected if otherwise suitable.
// 0 success; -EINVAL invalid options; -ENODEV no matching name/IPv4 record;
// -EADDRNOTAVAIL nonlocal IPv4; -ENETDOWN no eligible candidate;
// -ENOTUNIQ ambiguous candidates. Malformed snapshot rows are ineligible.
// All APIs preserve output on failure; allocation failure returns -ENOMEM.
int SelectNetworkInterface(const std::vector<NetworkInterfaceInfo>&,
                           const NetworkInterfaceOptions&, NetworkInterfaceSelection& output);
int ResolveNetworkInterface(const NetworkInterfaceOptions&, NetworkInterfaceSelection& output);
// Snapshots do not guarantee reachability, firewall acceptance or continued link
// availability. No route-metric preference, NIC monitoring, rebinding or service startup.
}
