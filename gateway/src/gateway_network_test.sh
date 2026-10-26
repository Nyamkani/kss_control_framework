#!/usr/bin/env bash
# Optional Linux integration tests. Requires unshare/user namespaces, iproute2,
# mount and dummy NIC support; no sudo, physical NIC or host network changes.
# Usage: bash gateway_network_test.sh /absolute/build/gateway
set -euo pipefail
build=$(realpath "${1:?build/gateway directory required}")
if [[ ${2:-} != --isolated ]]; then
    exec unshare --user --map-root-user --net --ipc --mount --fork --kill-child=KILL \
        bash "$0" "$build" --isolated
fi
ip link set lo up
mount -t tmpfs tmpfs /dev/shm
"$build/kcf_gateway_test" --network "$build/kcf_gateway"
"$build/kcf_gateway_test" --policy-network "$build/kcf_gateway"
state=$(mktemp -d /tmp/kcf-gateway-nic-XXXXXX)
child=
owner=
cleanup() {
    if [[ -n $child ]]; then kill -TERM "$child" 2>/dev/null || true; wait "$child" || true; fi
    if [[ -n $owner ]]; then kill -TERM "$owner" 2>/dev/null || true; wait "$owner" || true; fi
    rm -rf "$state"
}
trap cleanup EXIT
if ! ip link add n56a type dummy; then
    echo 'NIC integration unavailable: dummy interface creation failed' >&2
    exit 77
fi
ip addr add 192.0.2.1/24 dev n56a
ip link set n56a multicast on up
args=(--ipc-socket "$state/gateway.sock" --lock-file "$state/gateway.lock" --state-dir "$state" --discovery-port 47651 --control-port 49001 --topic-port 47652)
start() {
    "$build/kcf_gateway" "${args[@]}" "$@" > "$state/run.log" 2>&1 & child=$!
    for ((i=0;i<80;i++)); do
        if grep -q '\[Gateway\] ready' "$state/run.log"; then return; fi
        if ! kill -0 "$child" 2>/dev/null; then cat "$state/run.log"; return 1; fi
        sleep 0.1
    done
    cat "$state/run.log"; return 1
}
stop() { kill -TERM "$child"; wait "$child"; child=; }
start
grep 'interface=n56a.*automatic=1' "$state/run.log"
stop
ip link add n56b type dummy
ip addr add 198.51.100.1/24 dev n56b
ip link set n56b multicast on up
"$build/kcf_gateway" "${args[@]}" > "$state/ambiguous.log" 2>&1 & child=$!
for ((i=0;i<80;i++)); do
    if grep -q 'state=WAITING_NETWORK.*error=-76' "$state/ambiguous.log"; then break; fi
    sleep 0.1
done
grep 'state=WAITING_NETWORK.*error=-76' "$state/ambiguous.log"
stop
start --interface n56a
grep 'interface=n56a.*automatic=0' "$state/run.log"
stop
"$build/kcf_gateway_test" --owner > "$state/owner.log" 2>&1 & owner=$!
args+=(--topic-allow-prefix /gateway/recovery/ --topic-exclude-prefix /gateway/recovery/private/)
# Same process: absent NIC -> recovery -> down/up -> IPv4 change.
ip link set n56a down
"$build/kcf_gateway" "${args[@]}" --interface n56a > "$state/recovery.log" 2>&1 & child=$!
wait_pattern() {
    for ((i=0;i<100;i++)); do
        if grep -q "$1" "$state/recovery.log"; then return; fi
        if ! kill -0 "$child" 2>/dev/null; then cat "$state/recovery.log"; return 1; fi
        sleep 0.1
    done
    cat "$state/recovery.log";return 1
}
wait_pattern 'state=WAITING_NETWORK'
ip link set n56a up
wait_pattern '\[Gateway\] session='
first_session=$(sed -n 's/.*\[Gateway\] session=//p' "$state/recovery.log" | head -n1)
first_host=$(cat "$state/host-id")
sleep 2
baseline_fd=$(ls "/proc/$child/fd" | wc -l)
baseline_threads=$(ls "/proc/$child/task" | wc -l)
[[ $(grep -c 'state=RUNNING' "$state/recovery.log") == 1 ]]
for cycle in 1 2 3; do
    if [[ $cycle != 3 ]]; then ip link set n56a down; sleep 1.5; fi
    ip addr flush dev n56a
    ip addr add "192.0.2.$((cycle+1))/24" dev n56a
    ip link set n56a up
    wait_pattern "successful_restarts=$cycle"
    for ((j=0;j<100;j++)); do
        if [[ $(grep -c '\[Gateway\] session=' "$state/recovery.log") -ge $((cycle+1)) ]]; then break; fi
        sleep 0.1
    done
    sleep 0.2
    [[ $(ls "/proc/$child/fd" | wc -l) == "$baseline_fd" ]]
    [[ $(ls "/proc/$child/task" | wc -l) == "$baseline_threads" ]]
done
[[ $(cat "$state/host-id") == "$first_host" ]]
[[ $(grep -c 'topic=ON topic_max_hz=50 allow_prefixes=1 exclude_prefixes=1' "$state/recovery.log") == 4 ]]
[[ $(sed -n 's/.*\[Gateway\] session=//p' "$state/recovery.log" | sort -u | wc -l) == 4 ]]
[[ $(sed -n 's/.*\[Gateway\] session=//p' "$state/recovery.log" | tail -n1) != "$first_session" ]]
ip link set n56a down
sleep 1.5
stop
cat "$state/recovery.log"
kill -0 "$owner"
kill -TERM "$owner";wait "$owner";owner=
cat "$state/owner.log"
echo 'Gateway real NIC waiting/down/IP/recovery/session/FD/thread/signal PASS'
