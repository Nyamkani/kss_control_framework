#pragma once
#include "kcf/network/types.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include "kcf/dynamic/dynamic_payload.hpp"
namespace kcf::network {
inline constexpr std::size_t MAX_PARAMETER_WIRE = 16384;
// Role is endpoint-specific: Parameter/Topic VALUE, Service REQUEST/RESPONSE,
// Action REQUEST (goal), FEEDBACK, RESPONSE (result). NONE is never queryable.
enum class DescriptorRole : std::uint8_t { NONE=0, VALUE=1, REQUEST=2, RESPONSE=3, FEEDBACK=4 };
int DescriptorType(const NetworkEndpointMetadata&, DescriptorRole, TypeIdentity&);
// Schema v1, big-endian: u16 version; u16 type-name length + name bytes;
// u16 field count; each field: u16 name length + name bytes, u8 kind, u32 count.
// Names exclude NUL; type name 1..159 bytes, field name 1..95, fields 1..128.
// No native offsets, element sizes, alignment, padding or pointers on wire.
// Element sizes derive from PrimitiveSize; products are checked in uint64_t.
// Duplicate names, unknown kinds, zero counts and trailing bytes are rejected.
// Decode produces a packed buffer descriptor (alignment=1, cumulative offsets),
// NOT the remote or a C++ native layout. Use byte buffers and the existing value
// codec; never reinterpret_cast that buffer to an application struct.
// Names/field order/counts must reproduce the complete expected TypeIdentity.
// Output remains unchanged on error; schema and canonical values <=16 KiB.
int EncodeTypeDescriptor(const TypeDescriptor&, std::vector<std::uint8_t>&);
int DecodeTypeDescriptor(const std::vector<std::uint8_t>&, const TypeIdentity&, TypeDescriptor&);
// Fields in descriptor order, big-endian primitives, no offsets/padding on wire.
int ParameterWireType(const TypeDescriptor&, TypeIdentity&);
int EncodeParameterValue(const TypeDescriptor&, const DynamicPayload&, std::vector<std::uint8_t>&);
int DecodeParameterValue(const TypeDescriptor&, const std::vector<std::uint8_t>&, DynamicPayload&);
}
