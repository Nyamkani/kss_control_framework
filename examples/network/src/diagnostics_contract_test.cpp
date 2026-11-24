#include "kcf/network/topic_metrics.hpp"
#include "kcf/network/diagnostic_result.hpp"
#include "kcf/introspection/supervisor_diagnostics_info.hpp"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
using namespace std::chrono_literals;

int main() {
    namespace n=kcf::network;
    n::TopicRouteInfo before{};
    before.name="/test";
    before.source.runtime.host.bytes[0]=1;
    before.destination.runtime.host.bytes[0]=2;
    before.source_session=3;
    before.destination_session=4;
    before.route_id=5;
    before.packets_sent=100;
    before.packets_received=90;
    before.samples_published=88;
    before.dropped=3;
    before.missed_sequence=5;
    before.local_samples_skipped=10;
    auto after=before;
    after.packets_sent+=50;
    after.packets_received+=45;
    after.samples_published+=43;
    after.dropped+=2;
    after.missed_sequence+=4;
    after.local_samples_skipped+=5;
    n::TopicRouteRates rates{};
    assert(!n::ComputeTopicRouteRates(before,after,1000ms,rates));
    assert(rates.sent_hz==50 && rates.received_hz==45);
    assert(rates.proxy_publish_hz==43 && rates.dropped_delta==2);
    assert(rates.missed_sequence_delta==4 && rates.local_skipped_delta==5);
    assert(n::ComputeTopicRouteRates(before,after,0ms,rates)==-EINVAL);
    auto replaced=after;replaced.route_id++;
    assert(n::ComputeTopicRouteRates(before,replaced,1000ms,rates)==-ESTALE);
    replaced=after;replaced.packets_sent=99;
    assert(n::ComputeTopicRouteRates(before,replaced,1000ms,rates)==-ERANGE);
    assert(rates.received_hz==45); // preserve output on errors
    before.state=n::TopicRouteState::WAITING_SUBSCRIBER;
    assert(n::DiagnoseTopicRoute(before).code==n::DiagnosticCode::WAITING_SUBSCRIBER);
    before.state=n::TopicRouteState::REJECTED;
    before.reason=n::CompatibilityReason::TYPE_MISMATCH;
    assert(n::DiagnoseTopicRoute(before).code==n::DiagnosticCode::TYPE_MISMATCH);
    before.reason=n::CompatibilityReason::LOCAL_ONLY;
    assert(n::DiagnoseTopicRoute(before).code==n::DiagnosticCode::LOCAL_ONLY);
    before.reason=n::CompatibilityReason::COMPATIBLE;before.error=-EACCES;
    assert(n::DiagnoseTopicRoute(before).code==n::DiagnosticCode::PERMISSION_DENIED);
    before.error=0;
    assert(n::DiagnoseTopicRoute(before).code==n::DiagnosticCode::ROUTE_REJECTED);
    kcf::SupervisorDiagnosticsInfo supervisor{};
    assert(supervisor.element_count==0 && supervisor.history_count==0);
    assert(kcf::MAX_SUPERVISOR_DIAGNOSTIC_HISTORY==64);
    std::cout << "KCF diagnostics basic contract PASS\n";
}
