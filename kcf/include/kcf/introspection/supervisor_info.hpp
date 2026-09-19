#pragma once
#include "kcf/system/application_state.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace kcf
{
struct SupervisorElementInfo
{
    char name[128]{};
    char executable[241]{};
    std::uint8_t reserved0[3]{};
    std::int32_t pid{-1};
    std::uint64_t process_start_ticks{0};
    std::uint8_t process_alive{0};
    std::uint8_t reserved1[7]{};
};
inline constexpr std::size_t MAX_SUPERVISOR_ELEMENTS = 64;
struct SupervisorInfo
{
    std::uint32_t protocol_version{1};
    std::uint32_t struct_size{sizeof(SupervisorInfo)};
    std::int32_t pid{-1};
    std::uint32_t reserved0{0};
    std::uint64_t process_start_ticks{0};
    char application_name[128]{};
    ApplicationState application_state{ApplicationState::INITIALIZING};
    std::uint8_t reserved1[7]{};
    std::uint64_t revision{0};
    std::uint32_t element_count{0};
    std::uint32_t reserved2{0};
    SupervisorElementInfo elements[MAX_SUPERVISOR_ELEMENTS]{};
};
static_assert(std::is_trivially_copyable_v<SupervisorInfo>);
static_assert(std::is_trivially_copyable_v<SupervisorElementInfo>);
static_assert(sizeof(SupervisorElementInfo) == 392);
static_assert(sizeof(SupervisorInfo) == 176 + 392 * MAX_SUPERVISOR_ELEMENTS);
}
