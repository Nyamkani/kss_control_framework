#!/usr/bin/python3
"""Explicit, conservative setup and known-template updates of the host-wide Gateway."""
import argparse
import contextlib
import configparser
import difflib
import hashlib
import fcntl
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys
import tempfile

UNIT = 'kcf-gateway.service'
UNIT_DIR = Path('/etc/systemd/system')
STATE = Path('/var/lib/kcf')
RUNTIME = Path('/run/kcf')
ENV_FILE = Path('/etc/kcf/gateway.env')
PREFIX = Path(__file__).resolve().parent.parent
TEMPLATE = PREFIX / 'share/kcf/systemd/kcf-gateway.service.in'
EXECUTABLE = PREFIX / 'bin/kcf_gateway'

# Exact historical packaged templates, before account/path substitution. Never
# infer ownership from a filename, comment, or a subset of systemd directives.
KNOWN_TEMPLATES = frozenset({
    'ce8bad036a0fd307a741067d7b3d974fe0235d06968c06ca85af2322e2ea152e',  # #5-1/#5-2
    '6f8605c61cf446d43dbcf8509829911c057bf8db74f5be381a0cf2213723b580',  # empty args default
})


def read_unit(path):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_gid != 0
            or stat.S_IMODE(info.st_mode) != 0o644 or info.st_nlink != 1):
        fail(f'Unsafe existing unit/backup: {path}')
    return path.read_bytes().decode('utf-8')


def approved_previous(previous, rendered, account):
    normalized = previous.replace(f'User={account.pw_name}\n', 'User=@KCF_GATEWAY_USER@\n', 1)
    normalized = normalized.replace(f'ExecStart={EXECUTABLE} ', 'ExecStart=@KCF_GATEWAY_EXECUTABLE@ ', 1)
    if hashlib.sha256(normalized.encode()).hexdigest() not in KNOWN_TEMPLATES:
        fail('Unrecognized existing unit; administrator units are never overwritten.')
    def settings(text):
        config = configparser.ConfigParser(interpolation=None, strict=True)
        config.optionxform = str
        config.read_string(text)
        return config
    old, new = settings(previous), settings(rendered)
    # ExecStart includes the persistent state, singleton lock and IPC defaults.
    for key in ('User', 'Group', 'ExecStart', 'StateDirectory', 'RuntimeDirectory',
                'StateDirectoryMode', 'RuntimeDirectoryMode', 'EnvironmentFile'):
        if old.get('Service', key, fallback=None) != new.get('Service', key, fallback=None):
            fail(f'Update would change protected service setting: {key}')


def publish_unit(target, rendered, previous=None):
    """Publish without clobbering unknown files; retain an exact, durable backup."""
    fd, temporary = tempfile.mkstemp(prefix='.kcf-gateway-', dir=UNIT_DIR)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(rendered)
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
            os.fchown(stream.fileno(), 0, 0)
            os.fsync(stream.fileno())
        if previous is None:
            os.link(temporary, target)  # Exclusive initial creation.
        else:
            backup = target.with_name(target.name + '.backup-' + hashlib.sha256(previous.encode()).hexdigest())
            if backup.exists() or backup.is_symlink():
                if read_unit(backup) != previous:
                    fail('Existing backup conflicts; refusing update.')
            else:
                publish_unit(backup, previous)
            if read_unit(target) != previous:
                fail('Unit changed during setup; refusing update.')
            os.replace(temporary, target)
            print(f'Previous unit backed up: {backup}')
        directory_fd = os.open(UNIT_DIR, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def fail(message):
    raise RuntimeError(message)


def run(*args):
    result = subprocess.run(args, text=True, capture_output=True,
                            env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})
    if result.returncode:
        fail(f"{' '.join(args)} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def confirm(message):
    if input(message + ' [yes/NO]: ').strip() != 'yes':
        fail('Confirmation declined; no service changes made.')


def select_account(name, interactive):
    if not name:
        if not interactive:
            fail('Non-interactive setup requires --user USER.')
        name = os.environ.get('SUDO_USER')
        if not name:
            fail('No unambiguous sudo login account; specify --user USER.')
    if not re.fullmatch(r'[a-zA-Z_][a-zA-Z0-9_.-]*\$?', name):
        fail('Specify a login account name, not a numeric UID or unit syntax.')
    try:
        account = pwd.getpwnam(name)
    except KeyError:
        fail(f'Unknown account: {name}')
    uid_min = 1000
    for line in Path('/etc/login.defs').read_text().splitlines():
        match = re.match(r'^\s*UID_MIN\s+(\d+)\s*(?:#.*)?$', line)
        if match:
            uid_min = int(match[1])
    shells = {line.strip() for line in Path('/etc/shells').read_text().splitlines()
              if line.strip() and not line.startswith('#')}
    if (account.pw_uid == 0 or account.pw_uid < uid_min or account.pw_uid == 65534
            or account.pw_shell not in shells
            or Path(account.pw_shell).name in ('nologin', 'false')):
        fail('Gateway requires an ordinary non-root login account.')
    aliases = [entry.pw_name for entry in pwd.getpwall() if entry.pw_uid == account.pw_uid]
    if aliases != [account.pw_name]:
        fail(f'Ambiguous account names for UID {account.pw_uid}: {aliases}')
    print(f'Gateway account: {account.pw_name}, UID={account.pw_uid}, GID={account.pw_gid}')
    if interactive:
        confirm('Confirm this is the same UID used by the KCF Local Application Runtimes')
    return account


def safe_parents(path):
    for parent in reversed(path.parents):
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            fail(f'Unsafe parent directory: {parent}')


def inspect_tree(path, account):
    safe_parents(path)
    if not path.exists() and not path.is_symlink():
        return
    def inspect(item):
        info = item.lstat()
        if (stat.S_ISLNK(info.st_mode) or info.st_uid != account.pw_uid
                or info.st_gid != account.pw_gid or info.st_mode & 0o077):
            fail(f'Ownership/permissions conflict: {item}; no ownership changes allowed.')
        if stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
            fail(f'Hard-linked state/runtime file: {item}')
        if stat.S_ISDIR(info.st_mode):
            for child in item.iterdir():
                inspect(child)
        elif not (stat.S_ISREG(info.st_mode) or stat.S_ISSOCK(info.st_mode)):
            fail(f'Unsupported state/runtime entry: {item}')
    if not path.is_dir():
        fail(f'Not a directory: {path}')
    inspect(path)
    identity = path / 'host-id'
    if path == STATE and identity.exists() and not identity.is_file():
        fail('Host ID is not a regular file.')


def gateway_pids():
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if (proc / 'comm').read_text().strip() == 'kcf_gateway':
                found.append(int(proc.name))
        except FileNotFoundError:
            continue  # Process exited during inspection.
    return found


def inspect_service(account, rendered, allow_update=False):
    target = UNIT_DIR / UNIT
    safe_parents(target)
    previous = None
    if target.exists() or target.is_symlink():
        previous = read_unit(target)
        if previous != rendered:
            approved_previous(previous, rendered, account)
            if not allow_update:
                fail('Known older unit requires --update approval; inspect with --dry-run first.')
    props = dict(line.split('=', 1) for line in run(
        '/usr/bin/systemctl', 'show', UNIT, '--no-pager',
        '--property=LoadState,FragmentPath,DropInPaths,User,MainPID,ActiveState,UnitFileState,NeedDaemonReload'
    ).splitlines() if '=' in line)
    if props.get('LoadState') not in ('loaded', 'not-found'):
        fail(f'Unsupported existing unit state: {props}')
    if props.get('DropInPaths') or list((UNIT_DIR / (UNIT + '.d')).glob('*')):
        fail('Existing unit drop-ins require manual review.')
    if props.get('LoadState') == 'loaded':
        # A failed reload can leave the new, verified unit on disk while
        # systemd still holds the old definition. Only an exact current unit
        # may recover this way; all account/path/drop-in checks still apply.
        reload_pending = props.get('NeedDaemonReload') == 'yes' and previous == rendered
        if (props.get('FragmentPath') != str(target) or not target.exists()
                or props.get('User') != account.pw_name
                or (props.get('NeedDaemonReload') != 'no' and not reload_pending)):
            fail('Existing loaded service differs from the selected unit/account.')
    if props.get('UnitFileState', '') not in ('', 'disabled', 'enabled'):
        fail('Existing unit enablement state requires manual review.')
    active = props.get('ActiveState')
    if active not in ('active', 'inactive', 'failed'):
        fail(f'Service state is not stable: {active}')
    main_pid = int(props.get('MainPID', '0'))
    pids = gateway_pids()
    if active == 'active':
        if not target.exists() or main_pid == 0 or pids != [main_pid]:
            fail('Running Gateway conflicts with the existing system service.')
        proc = Path('/proc') / str(main_pid)
        if proc.stat().st_uid != account.pw_uid or (proc / 'exe').resolve() != EXECUTABLE.resolve():
            fail('Running Gateway UID/executable differs from the selected service.')
    elif pids or main_pid:
        fail('An unmanaged Gateway is running; no process will be stopped.')
    lock = RUNTIME / 'gateway.lock'
    if lock.exists():
        with lock.open('rb') as handle:
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                if active != 'active':
                    fail('Gateway lock is held outside the verified active service.')
    return props


def preflight(account, rendered, allow_update=False):
    for directory in (STATE, RUNTIME):
        inspect_tree(directory, account)
    if ENV_FILE.exists() or ENV_FILE.is_symlink():
        safe_parents(ENV_FILE)
        info = ENV_FILE.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            fail('Unsafe existing gateway.env; manual review required.')
        lines = [line.strip() for line in ENV_FILE.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        if any(line not in ('KCF_GATEWAY_ARGS=', 'KCF_GATEWAY_ARGS=""', "KCF_GATEWAY_ARGS=''")
               for line in lines):
            fail('Non-default gateway.env requires manual deployment review; it is preserved.')
    return inspect_service(account, rendered, allow_update)


def setup(args):
    interactive = sys.stdin.isatty()
    update = getattr(args, 'update', False)
    account = select_account(args.user, interactive)
    if not args.dry_run and os.geteuid() != 0:
        fail('Service configuration requires sudo/root; use --dry-run for inspection.')
    if not EXECUTABLE.is_file() or not os.access(EXECUTABLE, os.X_OK):
        fail(f'Installed Gateway executable is missing: {EXECUTABLE}')
    if not re.fullmatch(r'/[a-zA-Z0-9_/.-]+', str(EXECUTABLE)):
        fail('Executable path contains unsupported systemd syntax.')
    rendered = TEMPLATE.read_text().replace('@KCF_GATEWAY_USER@', account.pw_name).replace(
        '@KCF_GATEWAY_EXECUTABLE@', str(EXECUTABLE))
    if '@KCF_' in rendered:
        fail('Unresolved template placeholders.')
    with contextlib.ExitStack() as stack:
        # Serialize this tool without creating a lock file or touching Gateway's lock.
        if not args.dry_run:
            directory_fd = os.open(UNIT_DIR, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            stack.callback(os.close, directory_fd)
            fcntl.flock(directory_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        props = preflight(account, rendered, update or args.dry_run)
        with tempfile.TemporaryDirectory(prefix='kcf-gateway-verify-') as directory:
            candidate = Path(directory) / UNIT
            candidate.write_text(rendered)
            candidate.chmod(0o644)
            run('/usr/bin/systemd-analyze', 'verify', str(candidate))
        print(f'Validated unit: User={account.pw_name}, ExecStart={EXECUTABLE}, mode=0644')
        target = UNIT_DIR / UNIT
        previous = read_unit(target) if target.exists() else None
        if previous is not None and previous != rendered:
            print(''.join(difflib.unified_diff(previous.splitlines(True), rendered.splitlines(True),
                                             fromfile=str(target), tofile='verified new unit')), end='')
            if update and interactive and not args.dry_run:
                confirm('Approve backing up and updating this unit (no service restart)')

        if args.dry_run:
            print(rendered)
            print('DRY RUN: no unit/configuration/state changes; no reload, enable or start.')
            return
        if args.enable and interactive:
            confirm('Approve systemctl enable --now kcf-gateway.service')
        # Recheck after verification/prompts before changing the system.
        props = preflight(account, rendered, update)
        current = read_unit(target) if target.exists() else None
        if current != previous:
            fail('Unit changed during setup; refusing modification.')
        if previous is None or previous != rendered:
            publish_unit(target, rendered, previous)
            run('/usr/bin/systemctl', 'daemon-reload')
            print(f'Installed {target}; no service restart performed.')
        elif props.get('LoadState') == 'not-found' or props.get('NeedDaemonReload') == 'yes':
            # Retry only the reload; preserve the identical unit and its backup.
            run('/usr/bin/systemctl', 'daemon-reload')
        else:
            print('Identical unit already installed; preserved.')
        if args.enable:
            if props.get('UnitFileState') == 'enabled' and props.get('ActiveState') == 'active':
                print('Service already enabled and active; no activation needed.')
            else:
                run('/usr/bin/systemctl', 'enable', '--now', UNIT)
                print('Service enabled and started.')
        else:
            print('Activation not requested. Re-run with --user USER --enable to approve activation.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user', help='Explicit KCF login account (required without a terminal)')
    parser.add_argument('--dry-run', action='store_true', help='Check, verify and show unit/diff only; no service changes')
    parser.add_argument('--update', action='store_true',
                        help='Approve backup/update of an exact known older unit; does not restart')
    parser.add_argument('--enable', action='store_true', help='Explicitly approve enable --now (ignored in dry run)')
    args = parser.parse_args()
    try:
        setup(args)
    except (RuntimeError, OSError, ValueError, EOFError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
