# Gateway service lifecycle (NET56-3B, current NET56 integration)

Deployment template only: this repository does not install, enable or start the unit.
Framework version is 5.6.0; this alignment is not a formal release. No Debian packaging or Application Supervisor dependency.

## Account and rendering

Use the **same Linux UID as the Local Application Runtimes**. Local registry/Parameter/Topic
SHM is owner-only (`0600`), with read/write access required by existing shared synchronization.
An unrelated dedicated account is not supported by default. Do not chmod SHM or run as root
merely to bypass this boundary. Multiple Application UIDs need a separate permission design.
Use the same `/dev/shm` and PID visibility as the applications; containers/PrivateUsers or
private SHM mounts are not equivalent to same-UID host execution.

Render `kcf-gateway.service.in` by replacing `@KCF_GATEWAY_USER@` with that account name and
`@KCF_GATEWAY_EXECUTABLE@` with the absolute installed executable path. No implicit root default.
Review the rendered unit with `systemd-analyze verify /path/to/kcf-gateway.service`.
The account and binary must already exist. The deployment administrator may then place the
reviewed unit under `/etc/systemd/system/`, and optionally copy `gateway.env.example` to
`/etc/kcf/gateway.env`. These are instructions, not actions performed by this task.
Service activation is outside the manual test procedure; keep the service disabled. For an already running service,
`systemctl stop kcf-gateway.service` performs SIGTERM shutdown. Logs: `journalctl -u kcf-gateway.service`.

systemd creates `/var/lib/kcf` and `/run/kcf` owned by the selected UID, mode 0700.
Existing persistent identity must be reviewed before changing service account; do not overwrite
or regenerate it. StateDirectory ownership management can adjust existing ownership: account
migration requires stopping the service and checking the complete directory first.

## Lock and restart

Default lock is `/run/kcf/gateway.lock`; no `/tmp` fallback. Its parent must belong to the caller
and must not be group/world writable. All path components reject symlinks; the file must be a
single-link, owned regular file with mode 0600. Missing directories are explicit startup errors.
The runtime directory is retained on stop/restart to keep the lock inode stable. Never unlink
an active lock or run private runtime directories for supposedly identical production services.
The OS releases flock on normal exit or SIGKILL. A leftover file is normal, not a stale PID lock.
Development override deliberately creates a separate lock domain, so it cannot enforce a global
singleton against processes using another override. Production/manual launches must share the default.

Development, without systemd/root:

```bash
state=$(mktemp -d)
runtime=$(mktemp -d)
/path/to/kcf_gateway --state-dir "$state" --lock-file "$runtime/gateway.lock" \
  --ipc-socket "$runtime/gateway.sock"
```

The unit restarts abnormal exits after 3 seconds, at most 5 starts in 60 seconds.
Exit 2 (invalid arguments) is not restarted. Other process startup failures are retried only within that bound. NIC absence/ambiguity now stays
WAITING_NETWORK inside the Gateway; it does not itself exhaust systemd restart limits.
Permission/configuration or exhausted Network recovery errors can still leave the service failed.
Normal SIGTERM/SIGINT shutdown returns 0 and does not trigger Restart=on-failure. Explicit
`systemctl stop` does not auto-restart. SIGKILL/nonzero worker exits are distinguishable in the journal.
SIGTERM stops only this Gateway; after 15 seconds systemd may SIGKILL its cgroup if backend Stop
is blocked on an existing Local mutex. This does not repair another process's Local mutex.
Local Applications are not children/dependents of this unit and must not be placed in its cgroup.

Host ID survives restart; Discovery creates a new Session. Existing remote selections/Action handles
are not reattached or retried. With no exposure/reception options, Network default deny and Topic/Diagnostics OFF remain unchanged.
Session logs use `ControlNode::GetSessionId()`, the same source as Local IPC
`GET_STATUS`. A successful start prints `[Gateway] session=<nonzero ID>`;
WAITING_NETWORK/REBINDING state logs and IPC report zero without an active generation.
Restart/rebinding prints the new Session. No separate multicast observation socket,
packet decoding, or observation timeout is used for Session diagnostics.
NET56-3C adds NIC/IP polling and serialized Network rebinding with a new Session, while Local Runtime
ownership is unchanged. NET56-4 adds repeatable `--topic-allow-prefix` / `--topic-exclude-prefix` policies
and read-only access to exposed Publisher VALUE schemas. `--topic-rate NAME:HZ` (0.1..1000, default 50)
and `--topic-fresh-start NAME` grant no access; `--remote-topic NAME` is explicit legacy reception opt-in.
NET56-5 adds [read-only Local IPC](../LOCAL_IPC.md), default `/run/kcf/gateway.sock`, same UID only.
It remains available in WAITING_NETWORK and is not a Topic/Control relay. The development example above
must override the socket as well as the lock. Preserve the state directory across runs to preserve Host ID.
See the [current contract and verification boundary](../../docs/11_NETWORK_V56_DESIGN.md#0-현재-개발-구현과-검증-범위).
No Gateway failure automatically injects Local Runtime ERROR.

## Verification boundary

`kcf_gateway_test` covers configuration and secure lock validation. Optional
`bash gateway/src/gateway_network_test.sh /absolute/build/gateway` uses unprivileged Linux user,
network, IPC and mount namespaces, loopback/virtual NICs and a same-UID Local Parameter.
It verifies process restart and identity/session behavior, not the systemd restart controller.
A running systemd instance is required to test actual boot startup, restart limits, cgroup kill,
journal integration and automatic StateDirectory ownership. Static unit verification is not proof
of those behaviors. No service installation/enable is performed by the test.

## Manual Gateway with the service disabled

When the service is disabled, its RuntimeDirectory directive does not create /run/kcf.
An administrator can provision it independently with systemd-tmpfiles. Replace KCF_USER and
KCF_GROUP with the actual Local Runtime account and primary group. Do not broaden SHM permissions.
Create /etc/tmpfiles.d/kcf.conf with this content (administrator action, not performed here):

```text
d /run/kcf 0700 KCF_USER KCF_GROUP -
```

In an authorized administrative shell, apply only that configuration and inspect the result:

```sh
systemd-tmpfiles --create /etc/tmpfiles.d/kcf.conf
stat -c '%U %G %a' /run/kcf
systemctl is-enabled kcf-gateway.service
systemctl is-active kcf-gateway.service
```

Standard boot tmpfiles processing recreates /run/kcf after /run is cleared at reboot; it does not
start or enable the Gateway. Verify disabled/inactive service state and absence of an existing
manual Gateway before launching. Review existing ownership first; do not change accounts or
remove an active lock/socket to bypass a conflict. The '-' age requests no age-based cleanup.
Preserve the lock inode across manual restarts. If boot tmpfiles processing is unavailable,
an administrator must apply the configuration after boot.

Run the Gateway as the Local Runtime UID with the existing /var/lib/kcf/host-id, or a persistent
--state-dir override owned by that UID. The state directory must already exist; do not regenerate
Host ID. Default lock/socket paths use /run/kcf. This procedure includes no service activation
or firewall changes and does not validate automatic boot or restart behavior.

### 2026-10-28 physical LAN report is not a systemd test

Framework version alignment is 5.6.0, Tools 0.14.0. The user reported first-stage wired/Wi-Fi
50/100/200Hz reception on OPI5+. Actual systemd boot and automatic restart were not tested;
the service remained disabled. Discovery multicast gaps and IGMP Report recovery remain under
investigation. See [measurements and limits](../../docs/06_VERIFICATION_AND_LIMITATIONS.md#net56-lan-20261028).
Independent OPI5+ operational testing remains pending; Raspberry Pi 5 remains robot-only.
