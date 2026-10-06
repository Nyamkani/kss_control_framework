#pragma once
#include "kcf/dynamic/dynamic_payload.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include <memory>
#include <string>
namespace kcf {
namespace detail { class DynamicStorage; }
// Finish local operations before Open/Close/destruction. No automatic reconnect.
// Open binds this instance to the current named SHM object. Subsequent accesses
// return -ESTALE if it was unlinked/replaced, even with the same PID/type/layout.
// Retain the instance across polling; Close + Open explicitly selects a new object.
// Validation is a point-in-time namespace check, not an atomic unlink/write lock.
class DynamicParameterClient {
public:
    DynamicParameterClient();
    ~DynamicParameterClient();
    DynamicParameterClient(const DynamicParameterClient&) = delete;
    DynamicParameterClient& operator=(const DynamicParameterClient&) = delete;
    // Descriptor is required to check actual payload size/alignment/layout.
    int Open(const std::string& name, const TypeDescriptor& descriptor);
    int Open(const std::string& name, std::uint64_t expected_type_id,
             const TypeDescriptor& descriptor);
    int Get(DynamicPayload& value);
    int Set(const DynamicPayload& value);
    void Close() noexcept;
private:
    std::unique_ptr<detail::DynamicStorage> storage_;
};
}
