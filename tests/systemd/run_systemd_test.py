#!/usr/bin/env python3
"""Prepare by default; execute only in an explicitly approved disposable systemd guest."""
import argparse
import configparser
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import socket
import struct
import subprocess as sp
import sys
import time

UNIT = 'kcf-gateway-stab6b.service'
STATE = Path('/var/lib/kcf-stab6b')
RUNTIME = Path('/run/kcf-stab6b')
INSTALLED = Path('/run/systemd/system') / UNIT
ACK = 'I_APPROVE_DISPOSABLE_SYSTEMD_TEST'
MARKER_TEXT = 'KCF_STAB6B_DISPOSABLE_GUEST\n'
EXPECTED = {
    'Unit': {'StartLimitIntervalSec': '60', 'StartLimitBurst': '5',
             'Wants': 'network-online.target', 'After': 'network-online.target'},
    'Service': {'Type': 'exec', 'Restart': 'on-failure', 'RestartSec': '3',
                'RestartPreventExitStatus': '2', 'StateDirectory': 'kcf-stab6b',
                'RuntimeDirectory': 'kcf-stab6b', 'RuntimeDirectoryPreserve': 'yes',
                'StateDirectoryMode': '0700', 'RuntimeDirectoryMode': '0700',
                'UMask': '0077', 'KillMode': 'control-group', 'TimeoutStopSec': '15',
                'PrivateIPC': 'no', 'PrivateTmp': 'no', 'KillSignal': 'SIGTERM'}}


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def parse_unit(text):
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.optionxform = str
    config.read_string(text)
    return config


def render(template, binary, user):
    # Deliberately restrict paths/accounts to avoid systemd specifier/argument expansion.
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(binary)):
        raise ValueError('binary path must be absolute, without whitespace or systemd specifiers')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]*', user):
        raise ValueError('invalid service account')
    text = template.replace('@KCF_GATEWAY_USER@', user).replace('@KCF_GATEWAY_EXECUTABLE@', str(binary))
    text = text.replace('StateDirectory=kcf\n', 'StateDirectory=kcf-stab6b\n')
    text = text.replace('RuntimeDirectory=kcf\n', 'RuntimeDirectory=kcf-stab6b\n')
    text = text.replace('EnvironmentFile=-/etc/kcf/gateway.env\n', '')
    args = (f'{binary} --state-dir {STATE} --lock-file {RUNTIME}/gateway.lock '
            f'--ipc-socket {RUNTIME}/gateway.sock --interface lo --allow-loopback '
            '--discovery-port 47661 --control-port 49011 --topic-port 47662')
    text = re.sub(r'^ExecStart=.*$', 'ExecStart=' + args, text, flags=re.M)
    config = parse_unit(text)
    for section, values in EXPECTED.items():
        for key, value in values.items():
            if config.get(section, key, fallback=None) != value:
                raise ValueError(f'rendered unit mismatch: {section}.{key}')
    if config['Service']['User'] != user or 'EnvironmentFile' in config['Service']:
        raise ValueError('account/environment contract mismatch')
    if '@KCF_' in text or '$KCF_GATEWAY_ARGS' in text:
        raise ValueError('unresolved template')
    if config['Service']['ExecStart'] != args:
        raise ValueError('unexpected ExecStart')
    return text


def execution_guard(args):
    if not args.execute:
        return
    if args.approve_disposable_environment != ACK:
        raise ValueError('--execute requires the explicit disposable-environment approval phrase')
    if os.geteuid() != 0:
        raise ValueError('execute requires an already authorized root shell inside the guest; no sudo invoked')
    if not args.guest_marker:
        raise ValueError('execute requires --guest-marker')
    marker = Path(args.guest_marker)
    if marker.is_symlink() or not marker.is_file() or marker.stat().st_uid != 0 or marker.stat().st_mode & 0o022:
        raise ValueError('guest marker must be a root-owned, non-writable regular file, not a symlink')
    if marker.read_text() != MARKER_TEXT:
        raise ValueError('invalid disposable guest marker')
    if Path('/proc/1/comm').read_text().strip() != 'systemd':
        raise ValueError('PID 1 must be systemd')
    if sp.run(['systemd-detect-virt', '--quiet'], check=False).returncode:
        raise ValueError('no VM/container detected; physical hosts are refused')
    for path in (INSTALLED, Path('/etc/systemd/system') / UNIT,
                 Path(str(INSTALLED) + '.d'), Path('/etc/systemd/system') / (UNIT + '.d'), STATE, RUNTIME):
        if path.exists() or path.is_symlink():
            raise ValueError(f'pre-existing test resource refused: {path}')
    loaded = sp.run(['systemctl', 'show', UNIT, '--property=LoadState', '--value'],
                    text=True, stdout=sp.PIPE, stderr=sp.PIPE, timeout=10)
    if loaded.stdout.strip() != 'not-found':
        raise ValueError('test unit already known to systemd; execute refused')
    if not args.runtime_fixture or not Path(args.runtime_fixture).is_file():
        raise ValueError('--runtime-fixture must name the built kcf_gateway_test')
    # Never create a second Gateway beside an existing production/development one.
    for path in Path('/proc').glob('[0-9]*/comm'):
        try:
            if path.read_text().strip() == 'kcf_gateway':
                raise ValueError('an existing Gateway is running; execute refused')
        except (FileNotFoundError, PermissionError):
            continue


class Report:
    def __init__(self, out, args):
        self.out = out
        self.rows = []
        self.context = {'started': now(), 'arguments': vars(args), 'uid': os.getuid(),
                        'kernel': list(os.uname()), 'mode': 'execute' if args.execute else 'preparation'}
        self.flush()

    def add(self, name, status, detail, **extra):
        self.rows.append(dict(name=name, status=status, detail=detail, time=now(), **extra))
        self.flush()

    def flush(self):
        overall = 'FAIL' if any(r['status'] == 'FAIL' for r in self.rows) else 'PASS'
        if overall == 'PASS' and self.context['mode'] == 'execute' and any(r['status'] == 'SKIP' for r in self.rows):
            overall = 'SKIP'
        data = {'context': self.context, 'overall': overall, 'results': self.rows,
                'scope': 'Preparation PASS is not systemd operational validation PASS'}
        (self.out / 'results.json').write_text(json.dumps(data, indent=2) + '\n')
        lines = [f'# STAB-6B {self.context["mode"]}: {overall}', '',
                 'Preparation PASS does not certify service operation, boot, physical LAN or real-time behavior.', '',
                 '| Check | Result | Detail |', '|---|---|---|']
        lines += [f'| {r["name"]} | {r["status"]} | {r["detail"].replace(chr(10), " ").replace("|", "/")} |' for r in self.rows]
        lines += ['', '## First failures (never overwritten)', '']
        lines += [f'- {r["name"]}: {r["detail"]}' for r in self.rows if r['status'] == 'FAIL'] or ['None.']
        (self.out / 'RESULT.md').write_text('\n'.join(lines) + '\n')

    def command(self, label, command, timeout=30, check=True):
        begin = time.monotonic()
        log = self.out / f'{len(self.rows):03d}-{label}.log'
        try:
            result = sp.run(command, text=True, stdout=sp.PIPE, stderr=sp.STDOUT, timeout=timeout)
            output, code = result.stdout, result.returncode
        except sp.TimeoutExpired as e:
            output, code = str(e.output), -1
        log.write_text('$ ' + repr(command) + '\n' + output)
        self.add(label, 'PASS' if code == 0 else 'FAIL', str(log.name), command=command,
                 exit_code=code, elapsed=time.monotonic()-begin)
        if check and code:
            raise RuntimeError(f'{label} exit={code}')
        return output


def query(path):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(2)
        sock.connect(str(path))
        sock.sendall(struct.pack('!IIHHi', 0, 0x4b43464c, 1, 1, 0))
        def read(size):
            data = b''
            while len(data) < size:
                part = sock.recv(size-len(data))
                if not part:
                    raise ValueError('short IPC response')
                data += part
            return data
        length, magic, version, op, error = struct.unpack('!IIHHi', read(16))
        if length > 1048576 or (magic, version, op, error) != (0x4b43464c, 1, 1, 0):
            raise ValueError('invalid IPC header')
        value = json.loads(read(length))
        if not all(k in value for k in ('host', 'boot', 'session', 'state')):
            raise ValueError('missing IPC identity/state')
        return value


def until(predicate, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, ValueError):
            pass
        time.sleep(.1)
    raise RuntimeError('bounded condition wait timed out')


def live(args, report, rendered):
    account = pwd.getpwnam(args.user)
    owner = None
    installed_inode = None
    directory_inodes = {}
    started = now()
    def ctl(*words):
        return report.command('systemctl-' + words[0], ['systemctl', *words, UNIT])
    def show():
        output = ctl('show')
        return dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    def ipc_status():
        # SO_PEERCRED must see the same UID as Gateway, even with a root test driver.
        original = os.geteuid()
        try:
            os.seteuid(account.pw_uid)
            return query(RUNTIME / 'gateway.sock')
        finally:
            os.seteuid(original)
    def running():
        value = ipc_status()
        return value if value['state'] == 'RUNNING' and int(value['session']) else None
    def snapshot(label):
        state = show()
        pid = int(state['MainPID'])
        status = ipc_status()
        journal = report.command('journal-' + label, ['journalctl', '-u', UNIT, '--since', started, '--no-pager', '-o', 'cat'])
        if f'[Gateway] session={status["session"]}' not in journal:
            raise RuntimeError('journal/IPC Session mismatch')
        if owner.poll() is not None:
            raise RuntimeError('independent Runtime exited')
        details = {'pid': pid, 'ipc': status, 'fd_count': len(list(Path(f'/proc/{pid}/fd').iterdir())),
                   'threads': len(list(Path(f'/proc/{pid}/task').iterdir())), 'runtime_pid': owner.pid}
        report.add(label, 'PASS', 'journal/IPC session match; Runtime PID alive', **details)
        return details
    try:
        # Exclusive creation; never replace an existing unit, including symlinks.
        with INSTALLED.open('x') as f:
            f.write(rendered)
        installed_inode = INSTALLED.stat().st_ino
        report.command('daemon-reload', ['systemctl', 'daemon-reload'])
        log = open(report.out / 'local-runtime.log', 'w')
        fixture = str(Path(args.runtime_fixture).resolve())
        owner = sp.Popen([fixture, '--owner'], stdout=log, stderr=log, start_new_session=True,
                         user=account.pw_uid, group=account.pw_gid, extra_groups=[])
        log.close()
        until(lambda: 'Local owner ready' in (report.out / 'local-runtime.log').read_text())
        ctl('start'); until(running)
        for directory in (STATE, RUNTIME):
            st = directory.stat()
            directory_inodes[directory] = st.st_ino
            if st.st_uid != account.pw_uid or st.st_mode & 0o077:
                raise RuntimeError('systemd directory owner/mode mismatch')
        first = snapshot('initial-running')
        lock_inode = (RUNTIME / 'gateway.lock').stat().st_ino
        ctl('kill', '--kill-whom=main', '--signal=SIGKILL')
        until(lambda: running() and ipc_status()['session'] != first['ipc']['session'])
        second = snapshot('sigkill-restarted')
        if first['ipc']['host'] != second['ipc']['host'] or first['pid'] == second['pid']:
            raise RuntimeError('restart identity/PID contract')
        if second['fd_count'] != first['fd_count'] or second['threads'] != first['threads']:
            raise RuntimeError('restart FD/worker count changed')
        if Path(f'/proc/{first["pid"]}').exists():
            raise RuntimeError('old Gateway PID still present')
        # Duplicate is a separate, owned process in the same UID and lock domain.
        command = parse_unit(rendered)['Service']['ExecStart'].split()
        duplicate = sp.run(command, user=account.pw_uid, group=account.pw_gid, extra_groups=[],
                           text=True, stdout=sp.PIPE, stderr=sp.STDOUT, timeout=10)
        (report.out/'duplicate.log').write_text(duplicate.stdout)
        if duplicate.returncode != 1 or 'singleton lock error=-16' not in duplicate.stdout:
            raise RuntimeError('duplicate Gateway was not rejected')
        report.add('duplicate-lock', 'PASS', 'same-UID duplicate refused with -EBUSY')
        ctl('kill', '--kill-whom=main', '--signal=SIGTERM')
        until(lambda: show()['ActiveState'] == 'inactive')
        time.sleep(4)  # Observation exceeds the unchanged RestartSec=3 policy.
        state = show()
        if state['ActiveState'] != 'inactive' or state['MainPID'] != '0':
            raise RuntimeError('normal SIGTERM restarted')
        if (RUNTIME/'gateway.lock').stat().st_ino != lock_inode:
            raise RuntimeError('lock inode not preserved')
        report.add('sigterm-no-restart', 'PASS', 'inactive/MainPID=0 after restart window; lock inode retained')
        for repeat in range(2):
            ctl('reset-failed'); ctl('start'); until(running)
            snapshot(f'repeat-{repeat+1}')
            ctl('stop')
            if show()['MainPID'] != '0' or (RUNTIME/'gateway.sock').exists():
                raise RuntimeError('Stop retained process/socket')
        # Change only our transient test unit to exercise argument exit 2.
        invalid = re.sub(r'^ExecStart=.*$', 'ExecStart=' + str(args.gateway) + ' --invalid-stab6b-option', rendered, flags=re.M)
        INSTALLED.write_text(invalid)
        report.command('daemon-reload-invalid', ['systemctl', 'daemon-reload'])
        ctl('reset-failed'); ctl('start')
        until(lambda: show().get('ExecMainStatus') == '2')
        time.sleep(4)
        state = show()
        if state['ActiveState'] != 'failed' or state.get('NRestarts') != '0':
            raise RuntimeError('exit 2 restart prevention failed')
        report.add('exit-2-no-restart', 'PASS', 'ExecMainStatus=2, NRestarts=0')
        INSTALLED.write_text(rendered)
        report.command('daemon-reload-restored', ['systemctl', 'daemon-reload'])
        ctl('reset-failed'); ctl('start'); until(running)
        for crash in range(5):
            pid = show()['MainPID']
            ctl('kill', '--kill-whom=main', '--signal=SIGKILL')
            if crash < 4:
                until(lambda: show()['MainPID'] not in ('0', pid)); until(running)
        until(lambda: show().get('Result') == 'start-limit-hit')
        report.add('restart-limit', 'PASS', 'systemd Result=start-limit-hit after bounded crash sequence')
        owner.send_signal(signal.SIGTERM); owner.wait(timeout=10)
        text = (report.out/'local-runtime.log').read_text()
        if owner.returncode or 'Local Topic maintained commits=' not in text or 'exited normally PASS' not in text:
            raise RuntimeError('Local Runtime/Topic independence failed')
        report.add('runtime-independence', 'PASS', 'same independent PID through Gateway lifecycle; Local Topic assertions passed')
        report.add('metadata-route-rediscovery', 'SKIP', 'Requires a separate Discovery observer/remote consumer; GET_PEERS does not expose local endpoints')
    finally:
        if installed_inode is not None:
            try:
                ctl('stop')
                report.command('final-journal', ['journalctl', '-u', UNIT, '--since', started, '--no-pager'])
                final = show()
                if final['MainPID'] != '0':
                    raise RuntimeError('unit still owns a process; preserve resources for inspection')
                if INSTALLED.stat().st_ino != installed_inode:
                    raise RuntimeError('unit replaced externally; do not remove')
                INSTALLED.unlink()
                report.command('cleanup-daemon-reload', ['systemctl', 'daemon-reload'])
                for directory, inode in directory_inodes.items():
                    if directory.is_symlink() or directory.stat().st_ino != inode:
                        raise RuntimeError('test directory identity changed; preserve')
                    shutil.rmtree(directory)
                remaining = [str(d) for d in (STATE, RUNTIME) if d.exists()]
                if remaining:
                    raise RuntimeError('untracked test directories retained for inspection: ' + ', '.join(remaining))
                report.add('cleanup', 'PASS', 'only this invocation-owned unit/directories removed')
            except Exception as e:
                report.add('cleanup', 'FAIL', str(e))
        if owner is not None and owner.poll() is None:
            owner.terminate()
            try:
                owner.wait(timeout=10)
            except sp.TimeoutExpired:
                owner.kill(); owner.wait(timeout=5)
                report.add('runtime-cleanup', 'FAIL', 'owned fixture required SIGKILL; inspect its SHM manually')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--gateway', type=Path, required=True)
    parser.add_argument('--user', required=True, help='same non-root UID as test Local Runtime')
    parser.add_argument('--runtime-fixture', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--approve-disposable-environment', default='')
    parser.add_argument('--guest-marker')
    args = parser.parse_args()
    args.gateway = args.gateway.resolve()
    args.framework_root = args.framework_root.resolve()
    args.output_dir = args.output_dir.resolve()
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        parser.error('output directory must be empty; first results are never overwritten')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = Report(args.output_dir, argparse.Namespace(**{k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}))
    try:
        execution_guard(args)  # Before ANY unit installation/service control.
        account = pwd.getpwnam(args.user)
        if account.pw_uid == 0:
            raise ValueError('service/Runtime account must be non-root')
        if not args.gateway.is_file() or not os.access(args.gateway, os.X_OK):
            raise ValueError('existing executable Gateway required')
        template = (args.framework_root/'gateway/systemd/kcf-gateway.service.in').read_text()
        rendered = render(template, args.gateway, args.user)
        path = args.output_dir/UNIT
        path.write_text(rendered)
        report.add('rendered-contract', 'PASS', 'parsed directives/UID/paths/ports checked; production env excluded',
                   service_uid=account.pw_uid, template_sha256=hashlib.sha256(template.encode()).hexdigest())
        report.command('systemd-analyze', ['systemd-analyze', 'verify', str(path)])
        report.add('execution-gate', 'PASS', 'explicit approved disposable guest' if args.execute else 'dry-run: no systemctl/journalctl/NIC/service mutations')
        (args.output_dir/'PLAN.md').write_text('''# Operational plan
Only in an approved disposable systemd VM/container: start, SIGKILL/restart,
Host/Session comparison, duplicate lock, SIGTERM/no restart, repeated start/stop,
exit 2 prevention, five crashes/start-limit-hit, independent Local Runtime/Topic,
journal and IPC snapshots, invocation-owned cleanup.
Network dummy-NIC and boot validation are separate manual procedures in tests/systemd/README.md.
No production unit, /etc/kcf/gateway.env, /var/lib/kcf or /run/kcf is touched.
''')
        if args.execute:
            live(args, report, rendered)
        else:
            for name in ('lifecycle', 'restart-limit', 'runtime-independence', 'metadata-route-rediscovery', 'fd-worker-process'):
                report.add(name, 'SKIP', 'TEST PREPARATION only; no explicitly approved disposable systemd guest')
        report.add('network-recovery', 'SKIP', 'manual isolated dummy-NIC procedure; no physical NIC manipulation')
        report.add('boot-validation', 'SKIP', 'requires separately approved disposable VM reboot; never automated here')
    except Exception as e:
        report.add('failure', 'FAIL', str(e))
    print(f'Results: {args.output_dir / "RESULT.md"}')
    if (args.output_dir/'PLAN.md').exists():
        print((args.output_dir/'PLAN.md').read_text())
    return int(any(row['status'] == 'FAIL' for row in report.rows))


if __name__ == '__main__':
    sys.exit(main())
