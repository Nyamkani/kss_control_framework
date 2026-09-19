# R2B — Supervisor Application Discovery / Membership

Continues R1/R2A in `/tmp/kcf-r1-introspection`, branch `feature/introspection`.
No reset, commit or push. No kcf_tool or mecanum changes.

## Ownership and public view

Standalone:

```text
RuntimeInfo → EndpointRegistry
```

Supervised:

```text
SupervisorInfo → Element membership (PID + start ticks)
               → matching RuntimeInfo → EndpointRegistry
```

**Supervisor is an aggregator of application membership, not a Topic/Parameter data broker.**

Supervisor owns application name/state and configured membership. Element owns
runtime state and endpoint metadata. Actual IPC owns payloads/parameter values.
Supervisor never calls ListEndpoints and never copies Topic/Parameter lists.

`kcf/introspection/supervisor_info.hpp` provides trivially copyable
SupervisorInfo and SupervisorElementInfo. The snapshot includes the Supervisor's
PID/start ticks, display name, ApplicationState, revision and at most 64 members.
Members contain friendly name, configured executable, child PID/start ticks and
process_alive. Fixed arrays have explicit zero-initialized reserved bytes; compile-time
size checks verify the supported Linux layout has no unaccounted padding.

```cpp
kcf::IntrospectionClient client;
std::vector<kcf::SupervisorInfo> supervisors;
int result = client.ListSupervisors(supervisors);
```

The client scans only its internal prefix and validates filename generation against
/proc both before and after reading. It validates snapshot identity, protocol/size,
count, state enum, member flags and string termination. Invalid, stale, dead,
incomplete or inaccessible candidates are skipped. Global scan/allocation errors
return negative errno and preserve output; success replaces output, possibly empty.
It never unlinks stale objects. Child identity is the Supervisor's last management
observation, not independently certified by this read; match both PID and ticks
with ListRuntimes before following to ListEndpoints.

There is no atomic snapshot spanning all three registries. A process can exit
immediately after validation. PID/start ticks identify a process on this boot,
not a globally persistent identity or authentication credential.

## Bringup API and CLI

Existing `Setup(vector<ElementSpec>)` and `Setup(executable)` remain available.
The additional overload is:

```cpp
app.Setup("my_application", std::vector<bringup::ElementSpec>{ /* ... */ });
```

Old overloads and an empty explicit name use the current executable basename as a
diagnostic fallback (or `supervisor` if unavailable). Name lookup/allocation failure
does not fail Setup. Names are display metadata, not IPC addresses.

CLI accepts an optional leading name without consuming child arguments:

```sh
./build/bringup/kcf_bringup --application-name my_application executable [args...] --next executable [args...]
```

The existing executable-first syntax and timeout/element-name argument handling
remain intact. A later `--application-name` is left as a child argument.

After original SystemStatus initialization succeeds, Setup prepares introspection
and publishes configured names/executables before spawning. Successful spawn obtains
start ticks using the existing R1 helper. A failed generation lookup is metadata-only:
PID may be valid with ticks=0, which cannot be safely matched to RuntimeInfo.
Before spawn, identity is -1/0 and process_alive=0. On exit, the previous identity
remains diagnostic with process_alive=0. Reset clears old identities and then
publishes new generations with the same configured names.

## Storage and nonblocking control publication

Internal name: `/kcf_introspection_supervisor_<pid>_<start_ticks>`.
Storage: fixed `SharedChannel<SupervisorInfo>` with polling-only snapshot publication.

The existing SharedChannel::Publish is **not** suitable unchanged: it copies the
payload then locks notify_mutex and broadcasts. A paused observer holding that
mutex could block publication. Therefore R2B adds two narrow opt-in methods:

- `TryCreate`: uses nonblocking directory/object flocks for initialization.
  Ordinary Create and the default recovery helper retain their previous semantics.
- `TryPublishSnapshot`: the same slot/CAS/copy protocol, without notification mutex,
  condition broadcast, allocation, waiting or retry loops. It tries at most three
  slots and returns -EAGAIN if none is writable. Use ReadLatestSnapshot to poll;
  Wait/Subscriber notification semantics are not supported for this mode.

Existing Publish, Publisher behavior and normal notification semantics are unchanged.
No worker/service/thread is added to production Supervisor code. The new publication
path cannot wait for observer locks or acknowledgments. This is an observer-isolation
property, not a hard real-time guarantee against OS scheduling, page faults or system
resource delays. The read side can still spend time in Open/ReadLatestSnapshot and
belongs outside a GUI event thread.

SupervisorRegistry keeps an owning local desired snapshot. Revision increases only
when represented metadata changes, not on unchanged health polls or failed publication
retries. On -EAGAIN it keeps dirty state and retries at subsequent existing management
opportunities. Intermediate states may be skipped; this is a latest view, not an event
log. UINT64_MAX revision exhaustion stops accepting further changed snapshots.

Snapshot assembly/comparison is bounded to 64 members. It introduces bounded metadata
work on management cycles, but no introspection activity in Element::Loop/heartbeat
or Topic/Parameter data operations. No introspection result affects child spawn,
health timeouts, state transitions, Reset, Shutdown, SystemStatus or exit codes.
A creation failure disables this Supervisor's introspection for that lifecycle.

A killed reader may leave a slot pin behind under the existing channel algorithm.
If all usable slots become pinned, introspection may remain stale until owner restart;
Supervisor management still continues. R2B does not promise automatic abandoned-pin
recovery. Tests explicitly verify EAGAIN isolation and retry after fixture pins are
released. Normal Close/Unlink has no observer handshake and leaves existing reader
mappings to their own process lifetimes.

## Limits

- Only the first 64 configured elements appear. All configured children are still
  managed, including those beyond capacity. Changes solely to omitted members may
  not change the represented snapshot/revision unless application state changes.
- application/friendly names are truncated to 127 bytes, executables to 240 bytes,
  always NUL-terminated; byte truncation may split UTF-8. Friendly names can become
  indistinguishable after truncation; PID/generation is the live join key.
- SystemStatus protocol, channel and safety meaning are unchanged. SupervisorInfo
  does not replace it and contains no heartbeat or endpoint arrays.
- Topic/Parameter aggregation, R3 descriptors, Service introspection, dynamic read/
  monitor/write, Tool Interface and heartbeat observation remain deferred.

## Tests

```sh
cmake -S . -B build
cmake --build build -j 4
./build/examples/introspection/kcf_supervisor_discovery_test
```

Tests use real fork/exec processes: two named managed runtimes plus a separate
standalone runtime; one child exposes an R2A Publisher. They verify discovery,
application name, PID/generation matching, endpoint chain, standalone exclusion,
unchanged polling revision, Reset identity replacement and increased revision.

A test-only white-box channel fixture pins the slots and holds the legacy notify
mutex. While it remains paused, killing a managed child still produces ERROR on
the unchanged SystemStatus channel and Reset reaches RUNNING. After observer
SIGKILL and test-owned pin release, dirty membership publication catches up.
No production test hook is added.

Additional cases cover normal cleanup, dead Supervisor filtering without scanner
unlink, try-create under directory lock contention, 65 actual configured children
with only 64 reported, fallback naming and metadata creation collision without
Bringup failure. Regression entry points remain those listed in R1/R2A docs.

New CLI path test:

```sh
./build/examples/introspection/kcf_supervisor_discovery_test --cli ./build/bringup/kcf_bringup
```

## Result

Clean build PASS (C++17/GCC 13.3, no compiler warnings). R2B discovery/chain,
standalone separation, revision, pinned/dead observer isolation, Reset generations,
65-child capacity, metadata creation failure, cleanup/stale and new CLI tests PASS.
R1 discovery, R2A endpoints, Runtime lifecycle, runtime health/supervision,
Supervisor loss, all six Reset scenarios, SystemStatus/Topic/Parameter recovery,
both incomplete-initialization race suites and Integration PASS. Existing regression
scripts exercise the original Bringup CLI and API overloads. Final CLI test-only
refinement was rebuilt and tested; production code was unchanged after clean build.

SystemStatus public headers/storage/protocol are unchanged. Bringup invokes a
separate best-effort metadata update at existing publication opportunities; its
original SystemStatus result/error logic is unchanged. R1/R2A changes remain intact.
