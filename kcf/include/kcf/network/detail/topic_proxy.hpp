#pragma once
#include "kcf/dynamic/dynamic_payload.hpp"
#include "kcf/introspection/type_descriptor.hpp"
#include "kcf/network/detail/proxy_ownership.hpp"
#include <memory>
#include <string>
namespace kcf::network::detail {
// Gateway-only Format-4 writer. No endpoint registration: never a forwarding source.
// Destruction releases only its proven owned names. Existing reader mappings
// are never mutated/rebound; reconnect explicitly after a new incarnation.
class TopicProxy {
public:
    TopicProxy();~TopicProxy();
    int Create(const std::string&,const TypeDescriptor&,const ProxyOwnership&);
    ProxyRecoveryState RecoveryState() const { return state_; }
    bool StaleDetected() const { return stale_; }
    int Cleanup(); // best effort guarded name removal, also called by destructor
    int Publish(const DynamicPayload&); // nonblocking notify trylock + existing pin/CAS
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    ProxyRecoveryState state_{ProxyRecoveryState::NONE};bool stale_{false};
};
}
