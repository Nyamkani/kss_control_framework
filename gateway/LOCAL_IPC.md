# Gateway Local IPC v1 (NET56-5B1)

This is a read-only **host-local** protocol, separate from Network Discovery,
Control TCP, Topic wire and Local SHM formats. Framework version stays 5.5.0.
No second Discovery instance is created. No payload, Parameter Get/Set, Service,
Action, or remote-control relay operation is supported.

## Startup and ownership

`kcf_gateway --ipc-socket /path/to/private-runtime/gateway.sock` overrides the
production default `/run/kcf/gateway.sock`. The parent directory must already
exist, belong to the Gateway effective UID, and not be group/world writable.
Path components must be directories, not symlinks. Run Gateway with the same UID
as the Local Runtime and clients. Socket mode is 0600; every accepted connection
is also checked using Linux `SO_PEERCRED` against the Gateway effective UID.
This is a same-UID boundary, not per-application authorization or LAN authentication.

A separate `gateway.sock.lock` (0600, advisory exclusive flock) protects socket
ownership for the process lifetime. It does not replace the Host Gateway lock.
Lock files are never unlinked. A pre-existing socket is reclaimed only when it
belongs to the same UID, the socket lock is acquired, and a nonblocking connect
fails with ECONNREFUSED. Live sockets, symlinks, non-sockets, foreign ownership,
and ambiguous errors are rejected. Cleanup unlinks only the recorded socket
inode. SIGKILL leaves a reclaimable stale socket; normal Stop joins the worker,
closes connections/listener and removes the owned socket. No other SHM is deleted.

## Framing

AF_UNIX / SOCK_STREAM; one request and one response per connection. All header
integers are big endian, written explicitly (no native C++ structs).

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | Body length, excluding 16-byte header |
| 4 | 4 | Magic `0x4b43464c` (`KCFL`) |
| 8 | 2 | Version `1` |
| 10 | 2 | Operation: `1 GET_STATUS`, `2 GET_PEERS` |
| 12 | 4 | Signed errno-style result; requests must use zero |

Requests have length zero. Responses use the same operation. Result zero has a
JSON body; negative errno has no body. Unknown operations return `-ENOTSUP`.
Invalid header, version, nonzero request length/status, timeout, and connection
exhaustion close the connection. Clients must handle short reads and EOF, and
must not assume send/read correspond to complete frames. Pipelining is unsupported.

Limits: 8 concurrent clients, 32 admissions per second, backlog 8, one worker,
2 seconds from acceptance through complete response, response body <= 1 MiB.
Admission over the per-second budget waits in the bounded kernel backlog or fails;
there is no availability guarantee under overload. Oversized peer snapshots return
`-E2BIG` without silently truncating endpoints. A slow reader is disconnected.
Scheduling delays may exceed the nominal deadline; this is not a real-time service.

## Snapshot schema

GET_STATUS returns `state` (WAITING_NETWORK / REBINDING / RUNNING / ERROR),
`host`, `boot`, `interface`, `ipv4`, `interface_index`, `session`, `last_error`.
Host/Boot are 32-digit hex strings. Session is a decimal string, zero when no
Network generation exists. It comes directly from ControlNode's existing Discovery
session, not from multicast packet observation. `last_error` retains the latest
nonzero Network recovery error across successful recovery; initially zero.

GET_PEERS returns `{ "peers": [...] }`. Each peer contains `host`, `boot`,
`session`, `state` (ONLINE/LOST), `address`, `metadata_port`, `metadata_revision`,
`metadata_ready`, and `endpoints`. LOST retention/removal follows existing Discovery.
Each endpoint contains `runtime` (host/boot/pid/start_ticks), `registration_id`,
`application` (same Runtime identity schema), `host_name`, `application_name`,
`element_name`, `name`, `kind`, `role`, `scope`, `groups`, and
`value_type`, `request_type`, `response_type`, `feedback_type`.
Type identities contain `type_id`, `schema_id`, `encoding_id`, `payload_size`.
Kind/role/scope retain the numeric values of the existing Network metadata enums.
Unused identities/types remain zero; a zero Application identity means Standalone.

All 64-bit integer fields are decimal JSON **strings**, avoiding consumer floating
point loss. Metadata text is represented byte-for-byte: non-ASCII/control bytes
are escaped as `\u00XX`. To display non-ASCII text, reconstruct the original byte
string from these code points and decode using the application's text encoding.
This also permits lossless transfer of metadata not validated as UTF-8.

Gateway copies status and ControlNode::GetPeers() results to serialized snapshots,
normally every 100 ms and at lifecycle transitions. The IPC worker owns no
ControlNode pointer and never blocks the Network lifecycle on client I/O.
During blocking Network Stop/Start the latest published transition snapshot stays
available. Requests return snapshots, not an atomic transaction across separate
GET_STATUS/GET_PEERS calls. Recheck Session and current state before presenting
old selections as current. This read-only API confers no remote authority.

## Development verification

Build `kcf_gateway_local_ipc_test` and run it for codec/limits/path/FD checks.
The optional `src/local_ipc_integration.py BUILD_GATEWAY_DIR` runs in an isolated
user/network/IPC/mount namespace with iproute2 and dummy NIC support. It exercises
real Gateway waiting/rebinding, copied Peer metadata, Session changes and crash
recovery; it must not be run against host network interfaces. Existing
`src/gateway_network_test.sh` continues to check independent Local Runtime survival.
No systemd installation or physical LAN validation is implied by these tests.
