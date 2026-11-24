#pragma once
#include "kcf/network/topic_data_plane.hpp"
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>

namespace kcf::network {
// Transport-level observation only. These are NOT Local Publisher Hz,
// application consumption Hz, synchronized latency or source freshness.
// Two snapshots MUST come from the same route and both be measured locally.
struct TopicRouteRates {
    double sent_hz{0},received_hz{0},proxy_publish_hz{0};
    double drop_hz{0},missed_sequence_hz{0},local_skip_hz{0};
    std::uint64_t sent_delta{0},received_delta{0},proxy_publish_delta{0};
    std::uint64_t dropped_delta{0},missed_sequence_delta{0},local_skipped_delta{0};
    std::uint64_t elapsed_ms{0};
};
// Returns -ESTALE if a route was replaced, -ERANGE for counter reset, and
// -EINVAL for nonpositive intervals. Output remains unchanged on failure.
inline int ComputeTopicRouteRates(const TopicRouteInfo& previous,
                                  const TopicRouteInfo& current,
                                  std::chrono::milliseconds elapsed,
                                  TopicRouteRates& output)
{
    if(elapsed.count()<=0)return -EINVAL;
    if(previous.source!=current.source||previous.destination!=current.destination||
       previous.source_session!=current.source_session||
       previous.destination_session!=current.destination_session||
       previous.route_id!=current.route_id||previous.name!=current.name||
       current.route_id==0)return -ESTALE;
    if(previous.packets_sent>current.packets_sent||
       previous.packets_received>current.packets_received||
       previous.samples_published>current.samples_published||
       previous.dropped>current.dropped||
       previous.missed_sequence>current.missed_sequence||
       previous.local_samples_skipped>current.local_samples_skipped)return -ERANGE;
    TopicRouteRates r;
    r.elapsed_ms=static_cast<std::uint64_t>(elapsed.count());
    r.sent_delta=current.packets_sent-previous.packets_sent;
    r.received_delta=current.packets_received-previous.packets_received;
    r.proxy_publish_delta=current.samples_published-previous.samples_published;
    r.dropped_delta=current.dropped-previous.dropped;
    r.missed_sequence_delta=current.missed_sequence-previous.missed_sequence;
    r.local_skipped_delta=current.local_samples_skipped-previous.local_samples_skipped;
    const double factor=1000.0/static_cast<double>(elapsed.count());
    r.sent_hz=r.sent_delta*factor;
    r.received_hz=r.received_delta*factor;
    r.proxy_publish_hz=r.proxy_publish_delta*factor;
    r.drop_hz=r.dropped_delta*factor;
    r.missed_sequence_hz=r.missed_sequence_delta*factor;
    r.local_skip_hz=r.local_skipped_delta*factor;
    output=r;
    return 0;
}
} // namespace kcf::network
