#!/usr/bin/env bash
# No host bridge, routing, firewall, /dev/shm or daemon changes.
set -euo pipefail
if [[ ${1:-} == --help ]]; then
    echo "Usage: $0 [build-directory] [result-directory]"
    echo 'Requires Linux user/network/mount/IPC namespaces, iproute2/netem and C++17 build tools.'
    echo 'Runs rootless when unprivileged user namespaces are available; no sudo/password prompt.'
    exit 0
fi
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(realpath -m -- "${1:-$root/build}")
output=$(realpath -m -- "${2:-$build/network-virtual/$(date +%Y%m%d-%H%M%S)-$$}")
for tool in unshare nsenter ip tc ss mount python3 cmake; do
    if ! command -v "$tool" >/dev/null; then echo "LIMITATION: missing tool $tool" >&2; exit 77; fi
done
mkdir -p -- "$output"
echo "Results: $output"
if ! unshare --user --map-root-user --net --ipc --mount true 2>"$output/preflight.log"; then
    echo 'LIMITATION: user/network/IPC/mount namespace permission unavailable; see preflight.log.' >&2
    echo 'Enable user namespaces under your administrator policy, or run on a suitable Linux test host.' >&2
    exit 77
fi
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug >"$output/build.log" 2>&1
cmake --build "$build" --target kcf_virtual_host_fixture -j2 >>"$output/build.log" 2>&1
exec unshare --user --map-root-user --net --ipc --mount --pid --fork --kill-child=KILL --mount-proc \
    python3 "$root/tests/network_virtual/virtual_lan.py" --run "$build/examples/network/kcf_virtual_host_fixture" "$output"
