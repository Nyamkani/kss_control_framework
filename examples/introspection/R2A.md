# R2A Element Self-Description / Endpoint Registry

Continues R1 on `feature/introspection` at `/tmp/kcf-r1-introspection`.
R1's runtime identity/storage stays small and unchanged in layout.

## Public API

```cpp
kcf::IntrospectionClient client;
std::vector<kcf::RuntimeInfo> runtimes;
if (client.ListRuntimes(runtimes) == 0) {
    for (const auto& runtime : runtimes) {
        std::vector<kcf::EndpointInfo> endpoints;
        const int result = client.ListEndpoints(runtime, endpoints);
        // result == 0: endpoints is this runtime's current self-description.
    }
}
```

EndpointInfo is trivially copyable: registration_id, kind, role, payload_size,
name[241], diagnostic_type_name[160]. The diagnostic string is compiler-specific,
may be truncated, and is NOT a stable type identifier. It must never drive ABI
checks, casting, raw decoding, compatibility decisions or serialization.
No payload read/write, graph or Supervisor aggregation is provided.

ListEndpoints validates current PID/start ticks before access and again after
copying. It also validates protocol/size, snapshot identity, count, roles, IDs
(including duplicates), and string termination. It returns negative errno for
missing/stale/unavailable/invalid storage and preserves output on failure.
An empty active registry returns 0 with an empty vector. Runtime exit after the
last check is still possible; this is not a system-wide atomic snapshot.

## Ownership and lifecycle

Successful R1 Begin prepares a separate generation-matched endpoint registry
before Element::Setup. Its internal name is
`/kcf_introspection_endpoint_<pid>_<start_ticks>` and capacity is 128.
It uses SharedParameter directly, never the instrumented Parameter wrapper.
No active ProcessRuntime registry means registration is skipped; utility IPC
without ProcessRuntime continues to work.

Publisher registers after successful channel Create. Subscriber registers after
successful channel Open and worker creation. Parameter registers owner/client
roles after successful storage creation/open AND optional Watch setup.
Each wrapper unregisters its own ID only after its existing Close lifecycle.
Subscriber/Parameter early Close failures (including self-join) retain their ID.
Publisher destructor now performs the same Close/unregister cleanup.
Unlink semantics do not change; unlinking a name does not necessarily end an
already mapped local endpoint's lifetime.

IDs are process-local monotonic uint64 values and remain monotonic across repeated
Run calls in the same process generation, protecting new registrations from old
wrappers' delayed Close. ID 0 is invalid. Duplicate subscriptions have distinct IDs.
Local registration is mutex-serialized; metadata contains no owning containers.
Revision advances on registration/removal only. It does not change on Publish,
subscriber delivery, Get/Set, Loop or heartbeat. Revision is currently internal
snapshot metadata; no new public revision API was added.

Capacity/ID exhaustion and unavailable registry return ID 0 without changing IPC
results. On registry Set error, the owner abandons/unlinks the observational
registry for the rest of that Run. This avoids exposing a ghost registration if
SharedParameter reports a post-commit notification error. It does not tear down
any actual IPC endpoint. Runtime status remains separately available.

Normal runtime end closes and unlinks endpoint storage best-effort; wrapper Close
afterward is harmless. SIGKILL may leave stale storage, which client identity
validation excludes. There is no stale cleanup daemon. SharedParameter's existing
robust synchronization is reused; observation remains synchronous and does not
promise bounded latency for a live indefinitely paused mutex holder.

No additional registry access is present in Publish, subscriber callbacks,
Parameter Get/Set or ProcessRuntime Loop. Registration costs occur only at
endpoint/runtime lifecycle boundaries. Standalone and supervised processes use
the same self-description format and code path.

## Validation

```sh
cmake -S . -B build
cmake --build build -j 4
./build/examples/introspection/kcf_endpoint_registry_test
./build/examples/introspection/kcf_runtime_discovery_test
```

The R2A test uses a forked standalone ProcessRuntime and a parent-owned input topic.
It verifies all four roles, payload sizes, nonempty diagnostic strings, duplicate
subscription IDs and single-entry Close, capacity overflow with successful real
Parameter and Publisher operations, unchanged revision on data operations,
registry creation collision, mismatched generation/corrupt snapshot rejection,
observer SIGKILL and normal unlink of both registries.
A test-only pthread link wrapper injects ENOTRECOVERABLE at registry Set (outside
worker threads); Subscriber still creates/closes successfully, the failed metadata
registry disappears, and real Publisher/Parameter operations continue.
No production injection hooks are added. R1's test cleanup now removes its own
crash-created endpoint storage as well as runtime storage.

The R1 README documents the historical R1 scope; R2A additionally instruments
Publisher, Subscriber and Parameter lifecycle methods as described here.

## Deferred

R2B Supervisor aggregation; R3 stable type descriptors; dynamic Topic read;
dynamic Parameter get/set; Tool Interface; heartbeat observation. Supervisor,
kcf_tool and mecanum are not modified. No commit/push.

## Result

Clean build PASS with no compiler warnings. R2A endpoint test, R1 runtime discovery,
Runtime lifecycle, runtime health/supervision, Supervisor loss, all six Reset
scenarios, SystemStatus/Topic/Parameter recovery, incomplete-initialization races
(Topic and Parameter), and Integration all PASS. The final test-only registry
failure injection refinement was rebuilt and rerun successfully; production code
was unchanged during that refinement.

Composed features that already use Publisher/Subscriber internally (for example
an Action's status topic) naturally expose those underlying TOPIC endpoints.
There is no ACTION/SERVICE endpoint kind or action/service-level metadata API.
