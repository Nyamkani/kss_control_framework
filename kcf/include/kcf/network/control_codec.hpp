#pragma once
#include "kcf/network/validation.hpp"
#include <cerrno>

namespace kcf::network
{
// Legacy common-model operation tags: exactly two bytes,
// most significant byte first. Actual TCP framing uses control_frame.hpp and
// its distinct ControlMessage enum. This is not a request/result struct codec.
inline int EncodeControlOperation(ControlOperation operation, std::array<std::uint8_t, 2>& output)
{
    if (!Valid(operation)) return -EINVAL;
    const auto value = static_cast<std::uint16_t>(operation);
    output = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return 0;
}
inline int DecodeControlOperation(const std::uint8_t* bytes, std::size_t size, ControlOperation& output)
{
    if (!bytes) return -EINVAL;
    if (size != 2) return -EMSGSIZE;
    const auto value = static_cast<ControlOperation>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
    if (!Valid(value)) return -EPROTO;
    output = value;
    return 0;
}
} // namespace kcf::network
