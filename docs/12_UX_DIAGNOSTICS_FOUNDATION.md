# UX Diagnostics Foundation — Core #10 / #16 / #17 / #15 (Draft)

Status: **feature branch implementation; build, integration and physical LAN tests not yet run**.
Target: KCF Core v5.6.1 development branch. This is not a new release.

## 1. Supervisor details (Core #10)
The existing `SystemStatus` and `SupervisorInfo` v1 SHM formats are unchanged. A new independent
`SupervisorDiagnosticsInfo` SHM channel captures at most 64 Element statuses and 64 retained
errors, oldest first. Reader: `IntrospectionClient::GetSupervisorDiagnostics(pid,process_start_ticks,output)`.
The owner is the Supervisor only. The reader checks PID/start ticks before and after the snapshot,
schema/version, element bounds and error history sequence; this is **read-only** and cannot
restart, reset or kill Element processes.

Error entries contain local CLOCK_MONOTONIC nanoseconds, generation, sequence, failure kind,
runtime error and exit/signal details if known. Retention is KEEP_LAST; a sequence discontinuity
may mean overwritten events, but cannot prove a number of missing events without comparable
snapshots. A missing diagnostics channel is reported as N/A, not healthy/no error.
The read is point-in-time, not an atomic multi-Supervisor snapshot.

## 2. Topic transport rates (Core #16, partial)
`ComputeTopicRouteRates(previous,current,elapsed,output)` calculates **route-scoped** sent,
validated received, proxy-publish, dropped, missed sequence and skipped Local sample deltas/Hz,
using the same local monotonic measurement interval. The function explicitly rejects changed
route identities/sessions, nonpositive elapsed time and counter rollback. It does **not**
measure original Local Publisher Hz, application callback Hz, clock synchronization,
end-to-end latency or source freshness. Those require separate instrumentation and an
explicit observational definition.

## 3. Diagnostic evidence (Core #17, partial)
`DiagnoseTopicRoute(route)` maps the existing observed route state, compatibility reason and
error to structured code/stage values, e.g. `WAITING_SUBSCRIBER`, `LOCAL_ONLY`,
`TYPE_MISMATCH`, `PERMISSION_DENIED`, `PEER_LOST`. It does not claim that a failed
Discovery request proves Wi-Fi/IGMP, or that LOCAL_ONLY always means a Gateway allow-prefix
error. Unknown causes remain UNKNOWN. Other Endpoint kinds, Gateway state and complete
error guidance require later integration.

## 4. Read-only first CLI slice (Core #15, partial)
```sh
kcf status
kcf topic list
kcf network status
kcf doctor
```
`status`: Local Supervisor/Element latest failure/history when available; Local Runtime count.
`topic list`: currently registered Local Topic endpoint metadata; not proof of remote exposure.
`network status`: reads the existing same-UID Gateway Unix socket `/run/kcf/gateway.sock`
(GET_STATUS only), prints its JSON as returned; optional path argument for local testing.
`doctor`: basic local introspection discovery; flags Gateway/Remote diagnosis as not queried.
No new listener, Runtime Control, Network authorization, privilege escalation or process stop.
Topic info/echo/hz, complete `network status` peer view, stable machine JSON and doctor
recommendations remain **unimplemented** and their Issues stay open.

## Validation required before merging
- Ubuntu 24.04 amd64 Debug/Release and arm64 build with `kcf`, `kcf_diagnostics_contract_test`,
  `kcf_supervisor_failure_contract_test`, `kcf_supervisor_discovery_test`.
- Verify zero-init/no padding, SHM lifecycle, stale PID/ticks, full error buffer,
  Reset generation and overflow, multi-Element faults and no owner/control rights.
- CLI network same-UID/permission/timeout and Gateway missing/stale states;
  repeat query to catch descriptor race and count skipped/unknown.
- Data-plane counter rate interpretation on publisher restart, reconnect,
  dropped/duplicated packets, clock domain and source-vs-network counters.
- Ensure no performance regression on Supervisor monitor/control loop;
  OPI5+↔VM checks remain a separate physical validation.

Nothing in this draft closes Core #10/#15/#16/#17 or changes deployed packages.


## 2026-11-25 validation update

- Independent Draft PR #22 [Ubuntu 24.04 Actions #45381852680](https://github.com/Nyamkani/kss_control_framework/actions/runs/45381852680) PASS; combined Core [Actions #45400513845](https://github.com/Nyamkani/kss_control_framework/actions/runs/45400513845) PASS.
- VM `kcf_diagnostics_contract_test` **PASS**; `kcf_supervisor_failure_contract_test` **PASS** (deliberately injected startup failure, ERROR/RESET/cleanup, SIGKILL/timeout/fault paths); `kcf_supervisor_discovery_test` **PASS** (capacity 65, recovery/identity/generation, cleanup). ERROR lines are intentionally generated assertions of the failure-handling contract, not failures of these tests.
- VM new `build-ux/bin/kcf status`, `topic list`, `doctor`, `network status` exercised. Local status displayed 0 runtimes when empty and enumerated test fixture endpoints when running. `network status` successfully read existing VM systemd Gateway `/run/kcf/gateway.sock` and its RUNNING/session/NIC JSON, without replacing the systemd Gateway.
- Real OPI5+ (old distributed v5.6.1 Gateway) to VM Tools remote Subscribe/payload/49.08 Hz and Graph passed compatibility smoke, **not** physical validation of the new diagnostics implementation.
- Still required: new Core on ARM64, prolonged performance/SHM overhead and race tests, complete diagnostic and CLI commands. Issues #10/#15/#16/#17 remain OPEN; PR Draft.
