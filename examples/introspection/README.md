# R1 Public Runtime Discovery

Base: `dev` v4.3 (`234716b716f2f7e37c806bb9430dc9a9e11f74ab`).
Branch: `feature/introspection`. No commit/push.

## Public API

Include `kcf/introspection/introspection_client.hpp` and link `kcf_core`:

```cpp
kcf::IntrospectionClient client;
std::vector<kcf::RuntimeInfo> runtimes;
const int result = client.ListRuntimes(runtimes);
```

`RuntimeInfo` is a trivially copyable snapshot with protocol version/struct size,
PID + Linux process start ticks, executable basename, ExecutionMode, ProcessState
and runtime_error. It contains no pointer or owning container. The generation is
local to this running Linux system, not a persistent identity across reboots.
Names are safely truncated to 127 bytes plus NUL; truncation may split a multibyte
character, so consumers should tolerate non-UTF-8 process names.

ListRuntimes returns 0 and replaces output, including an empty vector. Negative
errno represents global enumeration/allocation failure and preserves output.
Malformed, incompatible, disappearing or inaccessible individual candidates are
skipped. Enumeration is best-effort, unordered, and not an atomic system snapshot.
Process exit immediately after the final validation is still possible.

The API is observational: it has no control, write, owner or stale-unlink method.
It internally uses SharedParameter Open/Get, which maps O_RDWR for synchronization;
“read-only” describes the public operation, not an OS read-only mapping/security
boundary. The metadata is local discovery information, not authentication.
ListRuntimes may block on low-frequency storage synchronization and should run
outside a GUI event thread. No Qt dependency is introduced.

## Internal behavior

One active ProcessRuntime per process remains the existing contract.
Each owns `/kcf_introspection_runtime_<pid>_<start_ticks>` using the unchanged
SharedParameter<RuntimeInfo>. The physical filename has no nested slash.
The prefix, `/proc` parsing, storage usage and owner lifecycle are detail-only.

The stat parser starts after the final `)` of comm and reads field 22. It accepts
spaces, parentheses and newlines in comm. Zombie/dead states are excluded.
The client checks generation before Open and again after Get; snapshot identity,
protocol, size, enum values and executable termination are validated as well.
It does not remove stale files. SIGKILL may leave an owner object behind.

Run captures launch mode before StartSupervision consumes its environment.
Begin publishes STARTING. Existing Run transitions publish RUNNING, STOPPING,
STOPPED or ERROR and the existing error code. Run's early failure returns also
clean up. End publishes the final state, Unlinks then Closes best-effort.
Final STOPPED/ERROR can be very brief before unlink; discovery is not an event log.

No update is performed per Loop, heartbeat or scheduler tick. No new lifecycle
state machine, worker, signal handling or supervision operation is added.
SupervisorLost precedence remains unchanged; its latched error is copied when
Run reaches its existing state/error transition. A blocked Setup/Loop can therefore
leave the previous observational snapshot visible until it returns. This is not
live health monitoring, and consumers must not interpret RUNNING as proof of progress.

All registry entry points are noexcept and discard storage failures; their results
never enter Runtime's return value. SharedParameter's robust mutex recovers a dead
reader without requiring an owner/observer ack. The test kills an observer while
it holds the actual storage mutex. This does not promise bounded latency against
an indefinitely paused/malicious reader holding that mutex; inherited synchronous
storage semantics are retained. Further observation lifecycle is deferred to R6.

## Validation

Clean build:

```sh
cmake -S . -B build
cmake --build build -j 4
./build/examples/introspection/kcf_runtime_discovery_test
```

The test uses real forked runtime/observer processes and link-time wrapping only
in the test executable. There are no production injection hooks. It covers:

- Standalone and supervised discovery, real comm with a space/parenthesis.
- PID/start ticks/executable and RUNNING state/error.
- Observer SIGKILL while holding SharedParameter's robust mutex, subsequent
  Runtime transition and normal cleanup.
- Dead and unreaped zombie filtering while retaining stale storage.
- Setup/Loop -EIO and ERROR snapshots without changing lifecycle result.
- Registry creation collision with a live parent-owned object; child reaches
  Element::Loop, returns 0 on SIGTERM, and does not modify the conflicting object.
- Protocol/size/snapshot/name generation and malformed string rejection;
  output replacement rather than append.

Test-created stale files are unlinked by that test's parent. Existing regression
programs that intentionally kill runtimes may leave R1 metadata as permitted;
there is no global stale cleaner.

Existing regression entry points:

```sh
./build/examples/supervisor/kcf_runtime_lifecycle_test
python3 examples/supervisor/runtime_health_checks.py build
python3 examples/supervisor/supervisor_loss_checks.py build
python3 examples/supervisor/reset_checks.py build
./build/examples/supervisor/kcf_system_status_recovery_test
./build/examples/recovery/kcf_topic_recovery_test
./build/examples/recovery/kcf_parameter_recovery_test
./build/examples/recovery/kcf_incomplete_recovery_test topic
./build/examples/recovery/kcf_incomplete_recovery_test parameter
```

Integration uses kcf_integration_backend plus kcf_integration_client, followed by
normal backend SIGTERM cleanup. Tests using fixed existing KCF channel names must
run serially in an environment without another application owning those names.

## Deferred

R2 endpoint registry, R3 flat type descriptor, R4 dynamic topic snapshot,
R5 dynamic parameter access, R6 further observation lifecycle and heartbeat live
observation. No application/element friendly names, Tool/UI changes, new daemon,
network discovery or changes to Publisher/Subscriber/Parameter<T>.

## R1 validation result

Clean build PASS (GCC 13.3, C++17, no new dependencies or compiler warnings).
Discovery, lifecycle, runtime health/supervision, Supervisor loss, all six reset
scenarios, SystemStatus/Topic/Parameter recovery, both incomplete-initialization
race tests and Integration PASS. Integration includes Topic/Timer, Service,
Parameter Get/Set/callback, Action success/cancel and concurrent operations.
The observer test also verifies owner-side robust recovery before any subsequent
client Get. Production code was unchanged during that final test refinement.

Original mecanum checkout and kcf_tool were not modified. Existing user changes
on mecanum remain in place. Public ProcessRuntime API, Publisher, Subscriber and
Parameter implementations were not modified.

R2B.1 scopes SystemStatus per Supervisor PID/start ticks. See [R2B1.md](R2B1.md) for child propagation, compatibility, and real KT-7 multi-Application validation.
