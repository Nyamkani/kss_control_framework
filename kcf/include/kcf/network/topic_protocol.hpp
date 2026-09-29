#pragma once
#include "kcf/network/validation.hpp"
namespace kcf::network {
inline constexpr std::uint32_t TOPIC_DATA_MAGIC=0x4b434654;
inline constexpr std::uint16_t TOPIC_DATA_VERSION=1;
inline constexpr std::size_t TOPIC_DATA_HEADER=184,MAX_TOPIC_DATAGRAM=1200,MAX_TOPIC_WIRE_PAYLOAD=768;
enum class TopicMessage : std::uint16_t { INTEREST=1, DATA=2 };
// INTEREST travels destination -> source; identities retain data direction.
struct TopicPacket {
    TopicMessage kind{TopicMessage::INTEREST};
    RemoteEndpointIdentity source,destination;
    std::uint64_t source_session{0},destination_session{0},route_id{0},sequence{0},sample_sequence{0};
    TypeIdentity type;std::string name;std::vector<std::uint8_t> payload;
};
int EncodeTopicPacket(const TopicPacket&,std::vector<std::uint8_t>&);
int DecodeTopicPacket(const std::uint8_t*,std::size_t,TopicPacket&);
}
