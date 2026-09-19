#!/usr/bin/env python3
"""Finite, local process tests. Run after building; no framework crash hooks."""
import os
from pathlib import Path
import re
import signal
import subprocess as sp
import sys
import tempfile
import time

build = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'build').resolve()
bringup = str(build / 'bringup/kcf_bringup')
examples = build / 'examples/supervisor'
runtime = str(examples / 'kcf_supervisor_runtime_test')
normal = str(examples / 'kcf_supervisor_normal')
safe = str(examples / 'kcf_supervisor_safe')
crash = str(examples / 'kcf_supervisor_crash')
probe = str(examples / 'kcf_runtime_supervision_test')


def no_shm(scope=""):
    suffix="_"+scope if scope else ""
    assert not Path("/dev/shm/kcf%2Fsystem%2Fstatus%2Fstate"+suffix).exists()


def child_sockets(pid):
    return sum(os.readlink(f).startswith('socket:') for f in Path(f'/proc/{pid}/fd').iterdir())


class Supervisor:
    def __init__(self, args):
        no_shm()
        self.log = tempfile.TemporaryFile(mode='w+')
        self.process = sp.Popen([bringup, *args], stdout=self.log, stderr=sp.STDOUT)
        self.start_ticks = Path(f"/proc/{self.process.pid}/stat").read_text().rsplit(")",1)[1].split()[19]
        self.started = time.monotonic()
        self.paused = []

    def text(self):
        self.log.seek(0)
        return self.log.read()

    def wait(self, text, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = self.text()
            if text in value:
                return value
            assert self.process.poll() is None, value
            time.sleep(.01)
        raise AssertionError(f'timed out waiting for {text}:\n{self.text()}')

    def pid(self, name):
        match = re.search(r'started name=' + re.escape(name) + r' pid=(\d+)', self.text())
        if match:
            return int(match[1])
        # Concurrent child stdout can split a Supervisor log line.
        children = Path(f'/proc/{self.process.pid}/task/{self.process.pid}/children').read_text().split()
        for pid in children:
            args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if b'--element-name' in args and args[args.index(b'--element-name') + 1].decode() == name:
                return int(pid)
        raise AssertionError(self.text())

    def stop(self, expected=None):
        for pid in self.paused:
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        self.process.wait(timeout=8)  # test stalls are finite; no supervisor escalation
        text = self.text()
        if expected is not None:
            assert self.process.returncode == expected, text
        for pid in re.findall(r'started name=\S+ pid=(\d+)', text):
            assert not Path('/proc/' + pid).exists(), text
        no_shm(f"{self.process.pid}_{self.start_ticks}")
        self.log.close()
        return text


def status(supervisor, kind, origin, error=0):
    out = sp.check_output([probe, '--read-system-status', str(supervisor.process.pid), supervisor.start_ticks], text=True)
    assert 'state=2' in out and f'failure_kind={kind}' in out and f'origin={origin} ' in out, out
    assert f'runtime_error={error}' in out, out
    if kind in (3, 4):
        assert 'termination=0' in out, out


# Standalone: no socket or supervision worker; control iterations still progress.
with tempfile.TemporaryFile(mode='w+') as log:
    p = sp.Popen([runtime, '--frequency', '1000'], stdout=log, stderr=sp.STDOUT)
    try:
        time.sleep(.4)
        assert len(list(Path(f'/proc/{p.pid}/task').iterdir())) == 1
        assert child_sockets(p.pid) == 0
    finally:
        p.send_signal(signal.SIGTERM)
        p.wait(timeout=3)
    log.seek(0)
    out = log.read()
    assert p.returncode == 0 and int(re.search(r'Shutdown loops=(\d+)', out)[1]) > 150, out
print('Standalone: one thread, no socket, 1kHz Loop progression PASS', flush=True)

s = Supervisor([normal, '--element-name', 'A', '--next', runtime, '--element-name', 'B',
                '--setup-delay-ms', '500', '--next', normal, '--element-name', 'C'])
try:
    s.wait('runtime name=B state=1')
    assert 'INITIALIZING -> RUNNING' not in s.text()
    s.wait('INITIALIZING -> RUNNING')
    assert time.monotonic() - s.started >= .5
    for name in ('A', 'B', 'C'):
        assert len(list(Path(f'/proc/{s.pid(name)}/task').iterdir())) == 2
        assert child_sockets(s.pid(name)) == 1  # sibling parent FDs did not leak across exec
    time.sleep(.4)
    assert ' -> ERROR' not in s.text()
finally:
    s.stop(0)
print('STARTING observation, all-RUNNING barrier, FD inheritance PASS', flush=True)

for args, marker in [(['--setup-result', '7'], 'startup barrier failed'),
                     (['--setup-delay-ms', '1500', '--startup-timeout-ms', '300'], 'startup timeout')]:
    s = Supervisor([normal, '--element-name', 'A', '--next', runtime, '--element-name', 'B', *args,
                    '--next', normal, '--element-name', 'C'])
    try:
        out = s.wait(marker)
        assert 'INITIALIZING -> RUNNING' not in out and 'Origin name=' not in out
        if '--setup-delay-ms' in args:
            assert 'runtime name=B state=1' in out
            assert time.monotonic() - s.started < 1.2
    finally:
        s.stop(1)
print('Setup failure + STARTING timeout -> cleanup (no ERROR latch) PASS', flush=True)

s = Supervisor([safe, '--element-name', 'safe', '--next', runtime, '--element-name', 'IMU',
                '--stall-after-loops', '20', '--stall-ms', '1800', '--health-timeout-ms', '300',
                '--next', crash, '--element-name', 'LiDAR', '--crash-after-ms', '1100'])
try:
    s.wait('Origin name=IMU')
    status(s, 4, 'IMU')
    assert Path(f'/proc/{s.pid("IMU")}').exists()
    s.wait('Enter SAFE state output=0')
    s.wait('unexpected exit name=LiDAR')
    status(s, 4, 'IMU')
    time.sleep(.4)
    assert s.process.poll() is None and 'stop name=IMU' not in s.text()
    assert s.text().count('Origin name=IMU') == 1 and 'ERROR -> RUNNING' not in s.text()
finally:
    out = s.stop(1)
    assert 'stop name=IMU' in out and 'ERROR -> SHUTTING_DOWN' in out
print('Heartbeat stall + SAFE + secondary crash + first origin latch + no auto kill PASS', flush=True)

s = Supervisor([safe, '--next', runtime, '--element-name', 'paused', '--health-timeout-ms', '300'])
try:
    s.wait('INITIALIZING -> RUNNING')
    pid = s.pid('paused')
    # Establish periodic Loop progress after the startup response; otherwise
    # a queued initial heartbeat=0 can legitimately trigger stall first.
    time.sleep(.4)
    os.kill(pid, signal.SIGSTOP)
    s.paused.append(pid)
    s.wait('Origin name=paused')
    status(s, 3, 'paused')
    s.wait('Enter SAFE state output=0')
    assert Path(f'/proc/{pid}').exists() and 'stop name=paused' not in s.text()
finally:
    s.stop(1)
print('SIGSTOP -> STATUS_TIMEOUT, SIGCONT only in test cleanup PASS', flush=True)

s = Supervisor([runtime, '--element-name', 'exception', '--throw-after-loops', '20', '--shutdown-delay-ms', '800'])
try:
    s.wait('Origin name=exception')
    status(s, 2, 'exception', -14)  # EFAULT for the test's Loop exception
finally:
    s.stop(1)
print('Runtime ERROR response while Shutdown in progress PASS', flush=True)

args = []
for index in range(20):
    if args:
        args.append('--next')
    args.extend([runtime, '--element-name', f'E{index}', '--frequency', '1000'])
s = Supervisor(args)
try:
    s.wait('INITIALIZING -> RUNNING')
    assert len(list(Path(f'/proc/{s.process.pid}/task').iterdir())) == 1
    for i in range(20):
        assert len(list(Path(f'/proc/{s.pid(f"E{i}")}/task').iterdir())) == 2
        assert child_sockets(s.pid(f'E{i}')) == 1
    time.sleep(1.5)
    assert ' -> ERROR' not in s.text()
finally:
    out = s.stop(0)
    counts = [int(v) for v in re.findall(r'Shutdown loops=(\d+)', out)]
    assert len(counts) == 20 and min(counts) >= 800, counts
print(f'20 Elements: single Supervisor thread, child worker=1, 1kHz loops min={min(counts)} PASS', flush=True)
print('Runtime health scenarios PASS', flush=True)
