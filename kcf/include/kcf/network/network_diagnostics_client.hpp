#pragma once
#include "kcf/network/peer_discovery.hpp"
#include "kcf/network/network_diagnostics.hpp"
#include <mutex>

namespace kcf::network {
// Blocking observer API: use a worker thread, never a GUI/event-loop thread.
// Borrows a running observer PeerDiscovery (DISCOVERY only, or receive-only
// Topic profile). Pass the exact config
// used for its Start(); discovery must outlive this client. Serialize discovery
// Start/Stop/destruction against Query. This API creates no listener or discovery.
// Use one active client per observer; Query calls on that client are serialized.
// IDs are process-wide, monotonic (including replacement client objects); do not
// fork and share a running discovery instance. Each Query opens/closes one TCP
// connection, with no automatic retry. The gateway may return -EAGAIN for rate
// limiting. It permits up to eight limited connections, so Action tracking and
// dedicated Control calls can coexist with this read-only connection.
class NetworkDiagnosticsClient {
public:
    NetworkDiagnosticsClient(PeerDiscovery& discovery, const DiscoveryConfig& config);
    NetworkDiagnosticsClient(const NetworkDiagnosticsClient&) = delete;
    NetworkDiagnosticsClient& operator=(const NetworkDiagnosticsClient&) = delete;
    // timeout: 1..60000 ms, one deadline including serialization/connect/HELLO/IO.
    // Returns 0 or negative errno (including gateway status). Snapshot unchanged
    // on failure. Rechecks discovery before dispatch and accepting the response.
    int Query(const HostIdentity& gateway, NetworkDiagnosticsSnapshot& snapshot,
              std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});
private:
    PeerDiscovery& discovery_;
    DiscoveryConfig config_;
    std::timed_mutex mutex_;
};
}
