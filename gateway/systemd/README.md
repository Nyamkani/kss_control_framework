# Gateway service lifecycle (NET56-3B, current NET56 integration)

The Core Debian package includes the `kcf-gateway-setup` command and deployment data: `/usr/share/kcf/systemd/kcf-gateway.service.in`
and `/usr/share/kcf/systemd/gateway.env.example`. CMake installs these unchanged under
`${CMAKE_INSTALL_DATADIR}/kcf/systemd`; account and executable placeholders remain unresolved.
Package installation alone does not install an active unit or `/etc/kcf/gateway.env`,
and does not enable or start a service.
Framework version is 5.6.1 (release preparation; no new release declared). No Application Supervisor dependency.

## Account and rendering

Use the **same Linux UID as the Local Application Runtimes**. Local registry/Parameter/Topic
SHM is owner-only (`0600`), with read/write access required by existing shared synchronization.
An unrelated dedicated account is not supported by default. Do not chmod SHM or run as root
merely to bypass this boundary. Multiple Application UIDs need a separate permission design.
Use the same `/dev/shm` and PID visibility as the applications; containers/PrivateUsers or
private SHM mounts are not equivalent to same-UID host execution.

### First account setup (#5-2)

The package installs `/usr/sbin/kcf-gateway-setup` (Python 3 standard library; `python3`
is an explicit DEB dependency). Run it on a host with systemd and the installed Gateway:

```sh
# Inspect only: no service, configuration or Host ID changes.
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --dry-run
# Install the verified unit, without enabling or starting it.
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER
# Explicit approval to enable and start the host service.
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --enable
```

Replace `KCF_USER` with the existing ordinary Linux login account used by the Local Application
Runtimes. In a terminal, omitting `--user` proposes `SUDO_USER`; the tool displays the account/UID
and requires typing `yes` before proceeding. It never infers an account from the build PC or root.
Interactive `--user` also requires confirmation. Without a terminal, `--user` is mandatory and
constitutes explicit account confirmation; `--enable` separately constitutes activation approval.
Numeric UIDs, root, unknown accounts, duplicate UID aliases, accounts below `UID_MIN`, and accounts
without a login shell listed in `/etc/shells` are rejected. The administrator must verify the
selected UID actually matches the intended Local Application Runtimes.

A non-root user may run `--dry-run` if all relevant state is readable. Inaccessible state or an
unavailable systemd bus is an error, never evidence that no service exists. Real setup requires
root. `--dry-run` renders the candidate in a temporary directory, runs `systemd-analyze verify`,
and prints the rendered unit; even with `--enable`, it performs no daemon reload or activation.
Temporary validation files are removed afterwards.

The tool checks `/var/lib/kcf` (including `host-id`) and `/run/kcf` recursively before changing
anything. Existing entries must have the selected UID and primary GID and owner-only permissions;
symlinks, hard-linked files and unsafe parent directories are rejected. Conflicts stop setup:
there is no chown, Host ID regeneration, lock/socket deletion or process termination.
An existing non-default `/etc/kcf/gateway.env` requires manual deployment review and is preserved;
the tool never sources it or copies the example over it. This conservative setup path does not
change CLI options, default-deny Topic policy, default lock or Local IPC paths.

The unit is rendered from the packaged template with the confirmed account and installed
`kcf_gateway` absolute path, then verified with `systemd-analyze verify`. A new unit is published
atomically as root:root mode 0644 at `/etc/systemd/system/kcf-gateway.service` without overwriting
an existing file. An identical existing unit is reused. Only an exact known older template can
be updated with `--update`; unknown units, drop-ins, masked or transitional service states,
and unmanaged running Gateways require manual review. A running
Gateway is reused only when it matches the active service's MainPID, selected UID and executable.
The tool checks the existing singleton lock without creating or deleting it. Concurrent invocations
of this setup tool are serialized; other administrative service/account changes must not run during
setup. Local applications must not start a manual Gateway during first setup.

Only `--enable` permits `systemctl enable --now kcf-gateway.service`; terminal use additionally
requires typing `yes` at the activation prompt before any service changes. An already enabled,
active service with an identical unit and no pending reload needs no reload or activation.
Failures are reported without automatic rollback or stopping services. A verified identical unit
left after a reload failure can be reused on retry, as described below. No package maintainer script invokes this tool.

Operate one common **system service** per host, with `User=` set to the KCF login account, shared by
Standalone and Supervised Applications with the same UID. Boot activation uses `multi-user.target`
and does not require that user to log in. It is not a per-user or per-Application service and has
no Supervisor lifetime dependency. Actual activation, boot and recovery tests are deferred to #5-3;
no enable/start is performed during repository validation. Logs: `journalctl -u kcf-gateway.service`.

systemd creates `/var/lib/kcf` and `/run/kcf` owned by the selected UID, mode 0700.
Existing persistent identity and complete directory ownership must be reviewed before the first
service start as well as any account change; do not overwrite or regenerate identity.
StateDirectory ownership management can adjust existing ownership, so a conflicting directory
must block setup before systemd starts the unit. Account migration requires a separate reviewed
procedure; it is not an automatic first-setup operation.

## Updating a deployed unit and recovering reload failure

After installing the new package, use the **existing** KCF account and installed binary path.
The following are administrator procedures, not commands executed during documentation validation:

```sh
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --dry-run
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --update
```

Dry-run shows the diff and runs `systemd-analyze verify` without writing a unit/backup or reloading.
`--update` explicitly approves replacement; interactive use also asks for confirmation. Recognition
uses exact historical template hashes after normalizing the selected account and executable path,
not a filename/comment heuristic. A recognized update must preserve User/Group, ExecStart (including
state/lock defaults), State/RuntimeDirectory and their modes, and EnvironmentFile. Administrator edits
and different accounts/paths are rejected rather than migrated.

Before replacement the verified old unit is backed up as
`/etc/systemd/system/kcf-gateway.service.backup-<SHA256-of-old-unit>`, root:root 0644.
The tool verifies the candidate, checks state again, publishes the unit atomically and calls
`systemctl daemon-reload`. An identical unit is not rewritten and no redundant backup is created.

If daemon-reload fails after replacement, the new unit and old backup remain on disk; systemd may
still have the old definition loaded (`NeedDaemonReload=yes`). Resolve the reported reload failure,
then run the same dry-run/update commands again. Recovery requires the disk unit to match the current
rendered template exactly and all account, path, ownership, drop-in, running-process and lock checks
to pass. It verifies the candidate again and retries **only daemon-reload**, without rewriting the
unit, backup, environment, Host ID or runtime files. Repeated failure is reported and leaves those
files intact. A successful retry followed by another invocation makes no redundant changes.
A unit not yet loaded (`LoadState=not-found`) also retains the existing initial-install retry path.
Unknown units or non-default `gateway.env` still require manual review; recovery does not bypass them.

Updating/reloading does not restart the running Gateway or load a new executable into that process.
An administrator must separately approve and schedule `sudo systemctl restart kcf-gateway.service`
when needed, then inspect `systemctl status kcf-gateway.service` and `journalctl -u kcf-gateway.service`.
Restart changes the Network Session and may interrupt remote operations; Host ID must remain unchanged.
`--enable` remains a separate approval for enable/start, not a request to restart an active service.

The template provides `Environment="KCF_GATEWAY_ARGS="` before the optional EnvironmentFile;
values in `/etc/kcf/gateway.env` take precedence. The setup tool does not create or overwrite that file.

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
Exit 2 (invalid arguments or singleton lock conflict `-EBUSY`) is not restarted. Other lock/runtime errors retain exit 1. Other process startup failures are retried only within that bound. NIC absence/ambiguity now stays
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

VM preparation records: setup regression **25/25 PASS** using mocked systemctl and temporary files,
with real `systemd-analyze verify`. Account tests exercise the actual selection function with mocked
OS account/config data (explicit user, SUDO_USER confirmation, root/system/unknown/numeric/duplicate
UID rejection). Update/reload failure, retry failure/success, dry-run and active-service preservation
are covered. These tests never enable/start/restart the development VM service.
Actual systemd reload recovery, boot/restart behavior, OPI5+ ARM64 and UGV02 rebuild/device operation
for **5.6.1** remain unverified. Earlier 5.6.0 package/device results are separate evidence.


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
