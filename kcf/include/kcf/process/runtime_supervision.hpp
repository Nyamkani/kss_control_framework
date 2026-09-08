#pragma once
#include "kcf/process/lifecycle.hpp"
#include <cstdint>
#include <type_traits>
namespace kcf
{
inline constexpr char SUPERVISION_FD_ENV[] = "KCF_SUPERVISION_FD";
inline constexpr std::uint32_t RUNTIME_STATUS_MAGIC = 0x4b435253;
inline constexpr std::uint16_t RUNTIME_STATUS_VERSION = 1;
struct RuntimeStatusRequest
{
    std::uint32_t magic{RUNTIME_STATUS_MAGIC};
    std::uint16_t protocol_version{RUNTIME_STATUS_VERSION};
    std::uint16_t packet_type{1};
    std::uint64_t request_id{0};
};
struct RuntimeStatusResponse
{
    std::uint32_t magic{RUNTIME_STATUS_MAGIC};
    std::uint16_t protocol_version{RUNTIME_STATUS_VERSION};
    std::uint16_t packet_type{2};
    std::uint64_t request_id{0};
    std::int32_t pid{-1};
    ProcessState state{ProcessState::STOPPED};
    std::uint8_t reserved[3]{};
    std::uint64_t loop_heartbeat{0};
    std::int32_t runtime_error{0};
    std::uint32_t reserved1{0};
};
static_assert(sizeof(RuntimeStatusRequest)==16 && sizeof(RuntimeStatusResponse)==40);
static_assert(std::is_trivially_copyable_v<RuntimeStatusRequest> && std::is_trivially_copyable_v<RuntimeStatusResponse>);
}
