#!/usr/bin/env python3
"""Run in isolated user/net/ipc/mount namespace; requires iproute2/dummy NIC."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess as sp
import sys
import tempfile
import time

build = Path(sys.argv[1]).resolve()
root = Path(tempfile.mkdtemp(prefix='kcf-local-ipc-live-'))
path = str(root / 'gateway.sock')
children = []
logs = []
def ip(*args):
    sp.run(['ip', *args], check=True)
def until(fn):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, AssertionError):
            pass
        time.sleep(.05)
    raise AssertionError('condition timed out')
def query(op):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(3)
        s.connect(path)
        s.sendall(struct.pack('!IIHHi', 0, 0x4b43464c, 1, op, 0))
        def read(n):
            data = b''
            while len(data) < n:
                part = s.recv(n-len(data))
                assert part
                data += part
            return data
        length, magic, version, operation, error = struct.unpack('!IIHHi', read(16))
        assert magic == 0x4b43464c and version == 1 and operation == op and error == 0 and length <= 1048576
        return json.loads(read(length))
def start():
    log = open(root / f'gateway-{len(logs)}.log', 'w')
    logs.append(log)
    p = sp.Popen([str(build/'kcf_gateway'), '--state-dir', str(root), '--lock-file', str(root/'gateway.lock'),
                  '--ipc-socket', path, '--interface', 'ipc0', '--discovery-port', '47651',
                  '--control-port', '49001', '--topic-port', '47652'], stdout=log, stderr=log)
    children.append(p)
    return p
def running():
    value = query(1)
    return value if value['state'] == 'RUNNING' and int(value['session']) else None
try:
    ip('link','add','ipc0','type','dummy')
    ip('link','set','ipc0','multicast','on')
    ip('addr','add','192.0.2.1/24','dev','ipc0')
    gateway = start()
    until(lambda: query(1)['state'] == 'WAITING_NETWORK')
    ip('link','set','ipc0','up')
    until(lambda: query(1)['state'] == 'REBINDING')
    first = until(running)
    peer = sp.Popen([str(build/'kcf_gateway_local_ipc_test'), '--peer'], stdin=sp.PIPE, stdout=sp.PIPE, text=True)
    children.append(peer)
    assert peer.stdout.readline().strip() == 'ready'
    def ready_peer():
        return next((p for p in query(2)['peers'] if p['metadata_ready'] and p['endpoints']), None)
    observed = until(ready_peer)
    assert observed['endpoints'][0]['name'] == '/ipc/peer'
    peer.stdin.write('remove\n');peer.stdin.flush()
    assert peer.stdout.readline().strip() == 'ok'
    until(lambda: any(p['metadata_revision'] != observed['metadata_revision'] and p['metadata_ready'] and not p['endpoints'] for p in query(2)['peers']))
    peer.stdin.write('quit\n');peer.stdin.flush();peer.wait(timeout=5)
    until(lambda: any(p['state'] == 'LOST' for p in query(2)['peers']))
    fd_count = len(list(Path(f'/proc/{gateway.pid}/fd').iterdir()))
    threads = len(list(Path(f'/proc/{gateway.pid}/task').iterdir()))
    ip('link','set','ipc0','down')
    until(lambda: query(1)['state'] == 'WAITING_NETWORK' and query(1)['session'] == '0')
    ip('addr','del','192.0.2.1/24','dev','ipc0');ip('addr','add','192.0.2.2/24','dev','ipc0');ip('link','set','ipc0','up')
    second = until(running)
    assert second['host'] == first['host'] and second['boot'] == first['boot'] and second['session'] != first['session'] and second['ipv4'] == '192.0.2.2'
    until(lambda: len(list(Path(f'/proc/{gateway.pid}/fd').iterdir())) == fd_count and len(list(Path(f'/proc/{gateway.pid}/task').iterdir())) == threads)
    gateway.terminate();assert gateway.wait(timeout=6) == 0;assert not Path(path).exists()
    gateway = start();third = until(running);gateway.kill();gateway.wait(timeout=5);assert Path(path).exists()
    gateway = start();fourth = until(running)
    assert third['host'] == fourth['host'] and third['session'] != fourth['session']
    gateway.terminate();assert gateway.wait(timeout=6) == 0;assert not Path(path).exists()
    print('Local IPC real Gateway WAITING/REBINDING/RUNNING, peer add/revision/LOST, session/IP change, FD/worker, SIGTERM/SIGKILL recovery PASS')
finally:
    for child in children:
        if child.poll() is None:
            child.kill();child.wait(timeout=5)
    for log in logs:
        log.close()
    print('Logs:', root)
