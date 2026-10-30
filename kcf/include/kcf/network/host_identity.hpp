#pragma once
#include "kcf/network/types.hpp"

namespace kcf::network {
// Caller-selected path; no default location, directory creation or service startup.
// File format: canonical 36-character UUID, optionally one trailing newline.
// Existing nonzero UUIDs are preserved (not required to be v4). New IDs are UUIDv4.
// Returns 0 or negative errno; output is unchanged on failure. Malformed files:
// -EINVAL; symlinks (including parent components): -ELOOP; nonregular files: -EINVAL.
// Parent directory must exist, be trusted and support flock/link/fsync (local Linux
// filesystem). '..' is rejected. All path components are opened without following
// symlinks. Parent-directory flock serializes cooperating callers across processes.
int LoadHostIdentity(const std::string& path, HostIdentity& output);
// Publishes a fully written, fsynced 0600 file by atomic no-replace hard link,
// then fsyncs the directory. Never replaces an existing file. Errors after publish
// may leave the complete ID installed; retry loads that ID, never regenerates it.
// Crash before publish can leave a .kcf-host-id-* temporary file; no automatic
// deletion of unrelated files. External replacement/deletion is not supported.
int LoadOrCreateHostIdentity(const std::string& path, HostIdentity& output);
// Reads /proc/sys/kernel/random/boot_id; independent from persistent Host ID.
int ReadLinuxBootIdentity(HostIdentity& output);
}
