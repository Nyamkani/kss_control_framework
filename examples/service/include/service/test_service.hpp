#pragma once
#include <cstdint>
#include <type_traits>

constexpr std::uint16_t SERVICE_ADD = 1;
constexpr std::uint16_t SERVICE_SET_VALUE = 2;
struct AddRequest { std::int32_t a; std::int32_t b; };
struct AddResponse { std::int32_t result; };
struct SetValueRequest { std::int32_t value; };
struct SetValueResponse { std::int32_t stored_value; std::uint32_t execution_count; };
static_assert(std::is_trivially_copyable_v<AddRequest> && std::is_trivially_copyable_v<AddResponse>);
static_assert(std::is_trivially_copyable_v<SetValueRequest> && std::is_trivially_copyable_v<SetValueResponse>);
