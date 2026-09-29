#pragma once
#include "kcf/network/types.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include "kcf/dynamic/dynamic_payload.hpp"
namespace kcf::network {
inline constexpr std::size_t MAX_PARAMETER_WIRE = 16384;
// Fields in descriptor order, big-endian primitives, no offsets/padding on wire.
int ParameterWireType(const TypeDescriptor&, TypeIdentity&);
int EncodeParameterValue(const TypeDescriptor&, const DynamicPayload&, std::vector<std::uint8_t>&);
int DecodeParameterValue(const TypeDescriptor&, const std::vector<std::uint8_t>&, DynamicPayload&);
}
