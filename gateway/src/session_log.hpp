#pragma once
#include "gateway_support.hpp"
#include <array>

namespace gateway {
// Diagnostic-only observation: ControlNode has no public local-session getter.
// No second Discovery, no outgoing packets, no state/authorization decisions.
class SessionLog {
    Fd socket_;
    kcf::network::DiscoveryConfig config_;
public:
    int Start(const kcf::network::DiscoveryConfig& config) {
        config_=config;socket_.value=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(socket_.value<0)return -errno;
        int one=1,zero=0;
        if(setsockopt(socket_.value,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)))return -errno;
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(config.port);
        if(bind(socket_.value,reinterpret_cast<sockaddr*>(&local),sizeof(local)))return -errno;
        ip_mreq group{};
        if(inet_pton(AF_INET,config.multicast_group.c_str(),&group.imr_multiaddr)!=1||
           inet_pton(AF_INET,config.interface_address.c_str(),&group.imr_interface)!=1)return -EINVAL;
        if(setsockopt(socket_.value,IPPROTO_IP,IP_ADD_MEMBERSHIP,&group,sizeof(group))||
           setsockopt(socket_.value,IPPROTO_IP,IP_MULTICAST_ALL,&zero,sizeof(zero)))return -errno;
        return 0;
    }
    std::uint64_t Poll() {
        for(int i=0;i<32;++i) {
            std::array<std::uint8_t,kcf::network::DISCOVERY_MAX_DATAGRAM> bytes{};
            sockaddr_in source{};socklen_t size=sizeof(source);
            auto count=recvfrom(socket_.value,bytes.data(),bytes.size(),MSG_DONTWAIT|MSG_TRUNC,reinterpret_cast<sockaddr*>(&source),&size);
            if(count<0)return 0;
            if(static_cast<std::size_t>(count)>bytes.size())continue;
            in_addr ip{};inet_pton(AF_INET,config_.interface_address.c_str(),&ip);
            if(source.sin_addr.s_addr!=ip.s_addr||ntohs(source.sin_port)!=config_.metadata_port)continue;
            kcf::network::DiscoveryPacket packet;
            if(kcf::network::DecodeDiscoveryPacket(bytes.data(),count,packet))continue;
            if(packet.kind==kcf::network::DiscoveryMessage::ANNOUNCE&&packet.host==config_.host&&packet.boot_id==config_.boot_id)return packet.session_id;
        }
        return 0;
    }
    void Close(){if(socket_.value>=0){close(socket_.value);socket_.value=-1;}}
};
}
