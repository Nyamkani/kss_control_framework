#pragma once
#include <cstdint>
#include <type_traits>
namespace kcf
{
enum class EndpointKind : std::uint8_t { TOPIC = 1, PARAMETER = 2 };
enum class EndpointRole : std::uint8_t {
    PUBLISHER = 1, SUBSCRIBER = 2, PARAMETER_OWNER = 3, PARAMETER_CLIENT = 4
};
struct EndpointInfo
{
    std::uint64_t registration_id{0};
    EndpointKind kind{};
    EndpointRole role{};
    std::uint64_t payload_size{0};
    std::uint64_t type_id{0}; // 0: no registered stable descriptor
    char name[241]{};
    // Diagnostic only. Not a stable type identifier. Never use for ABI,
    // compatibility checks, decoding, casts or serialization.
    char diagnostic_type_name[160]{};
};
static_assert(std::is_trivially_copyable_v<EndpointInfo>);
}
