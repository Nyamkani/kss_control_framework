#!/usr/bin/env python3
"""Setup regression: temporary files and mocked systemctl only; never changes services.

Run: python3 -B -m unittest discover -s tests/systemd -p test_gateway_setup.py -v
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import pwd
import stat
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / 'gateway/systemd'
SPEC = importlib.util.spec_from_file_location('gateway_setup', SOURCE / 'kcf-gateway-setup.py')
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.account = pwd.getpwuid(os.getuid())
        for name, value in dict(UNIT_DIR=self.root/'units', STATE=self.root/'state',
                                RUNTIME=self.root/'runtime', ENV_FILE=self.root/'env',
                                TEMPLATE=SOURCE/'kcf-gateway.service.in',
                                EXECUTABLE=Path('/proc/self/exe').resolve()).items():
            self.stack.enter_context(patch.object(setup, name, value))
        setup.UNIT_DIR.mkdir()
        self.target = setup.UNIT_DIR/setup.UNIT
        self.rendered = setup.TEMPLATE.read_text().replace('@KCF_GATEWAY_USER@', self.account.pw_name).replace(
            '@KCF_GATEWAY_EXECUTABLE@', str(setup.EXECUTABLE))
        self.previous = self.rendered.replace('Environment="KCF_GATEWAY_ARGS="\n', '')
        self.stack.enter_context(patch.object(setup, 'select_account', return_value=self.account))
        self.stack.enter_context(patch.object(setup, 'safe_parents'))  # Fixture is under /tmp.
        self.stack.enter_context(patch.object(setup.os, 'geteuid', return_value=0))
        self.stack.enter_context(patch.object(setup.os, 'fchown'))  # No actual ownership changes.
        self.stack.enter_context(patch.object(setup.sys.stdin, 'isatty', return_value=False))
        self.stack.enter_context(patch.object(setup, 'gateway_pids', return_value=[]))
        original = Path.lstat
        def root_owned(path):
            result = original(path)
            if path.parent == setup.UNIT_DIR or path == setup.ENV_FILE:
                values = list(result)
                values[4] = values[5] = 0
                return os.stat_result(values)
            return result
        self.stack.enter_context(patch.object(Path, 'lstat', root_owned))
        self.props = dict(LoadState='not-found', FragmentPath='', DropInPaths='', User='',
                          MainPID='0', ActiveState='inactive', UnitFileState='', NeedDaemonReload='no')
        self.calls = []
        self.reload_failures = 0
        actual_run = setup.run
        def run(*args):
            self.calls.append(args)
            if args[0] == '/usr/bin/systemd-analyze':
                return actual_run(*args)  # Real static verifier; no unit installation.
            self.assertEqual(args[0], '/usr/bin/systemctl')
            if args[1] == 'show':
                return '\n'.join(f'{k}={v}' for k, v in self.props.items())
            if args[1] == 'daemon-reload':
                if self.reload_failures:
                    self.reload_failures -= 1
                    self.props['NeedDaemonReload'] = 'yes'
                    raise RuntimeError('daemon-reload failed')
                self.loaded()
                return ''
            if args[1:3] == ('enable', '--now'):
                return ''
            self.fail(f'Unexpected service command: {args}')
        self.stack.enter_context(patch.object(setup, 'run', side_effect=run))
        self.output = self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def loaded(self):
        self.props.update(LoadState='loaded', FragmentPath=str(self.target), User=self.account.pw_name,
                          UnitFileState=self.props.get('UnitFileState') or 'disabled', NeedDaemonReload='no')

    def existing(self, text):
        self.target.write_text(text)
        self.target.chmod(0o644)
        self.loaded()

    def invoke(self, **kwargs):
        args = dict(user=self.account.pw_name, update=False, dry_run=False, enable=False)
        args.update(kwargs)
        setup.setup(argparse.Namespace(**args))

    def mutations(self):
        return [c for c in self.calls if c[0].endswith('systemctl') and c[1] != 'show']

    def backups(self):
        return list(setup.UNIT_DIR.glob('*.backup-*'))

    def test_first_install_and_identical_rerun(self):
        self.invoke()
        self.assertEqual(self.target.read_text(), self.rendered)
        self.assertEqual(stat.S_IMODE(self.target.stat().st_mode), 0o644)
        inode = self.target.stat().st_ino
        self.calls.clear()
        self.invoke(update=True)
        self.assertEqual(self.target.stat().st_ino, inode)
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.backups(), [])

    def test_update_requires_explicit_approval(self):
        self.existing(self.previous)
        with self.assertRaisesRegex(RuntimeError, '--update'):
            self.invoke()
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(self.mutations(), [])

    def test_update_backup_and_repeat_preserve_identity(self):
        self.existing(self.previous)
        setup.STATE.mkdir(mode=0o700)
        identity = setup.STATE/'host-id'
        identity.write_bytes(b'existing-host-id')
        identity.chmod(0o600)
        before = identity.stat()
        self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.rendered)
        self.assertEqual(len(self.backups()), 1)
        self.assertEqual(self.backups()[0].read_text(), self.previous)
        self.assertEqual(stat.S_IMODE(self.backups()[0].stat().st_mode), 0o644)
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])
        self.assertEqual(identity.read_bytes(), b'existing-host-id')
        self.assertEqual(identity.stat().st_mtime_ns, before.st_mtime_ns)
        self.calls.clear()
        self.invoke(update=True)
        self.assertEqual(self.mutations(), [])
        self.assertEqual(len(self.backups()), 1)

    def test_dry_run_diff_no_backup_or_control(self):
        self.existing(self.previous)
        self.invoke(dry_run=True, enable=True)
        self.assertIn('+Environment="KCF_GATEWAY_ARGS="', self.output.getvalue())
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(self.backups(), [])
        self.assertEqual(self.mutations(), [])

    def test_reload_failure_retry_failure_then_success_preserves_files(self):
        self.existing(self.previous)
        setup.STATE.mkdir(mode=0o700)
        setup.RUNTIME.mkdir(mode=0o700)
        identity = setup.STATE/'host-id'
        identity.write_bytes(b'existing-host-id')
        identity.chmod(0o600)
        setup.ENV_FILE.write_text('KCF_GATEWAY_ARGS=""\n')
        setup.ENV_FILE.chmod(0o644)
        def unchanged(path):
            info = path.stat()
            return (path.read_bytes() if path.is_file() else None, info.st_ino,
                    info.st_mtime_ns, info.st_mode, info.st_uid, info.st_gid)
        protected = {p: unchanged(p) for p in (identity, setup.STATE, setup.RUNTIME, setup.ENV_FILE)}
        self.reload_failures = 2
        with self.assertRaisesRegex(RuntimeError, 'daemon-reload failed'):
            self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.rendered)
        self.assertEqual(self.props['NeedDaemonReload'], 'yes')
        self.assertEqual(len(self.backups()), 1)
        self.assertEqual(self.backups()[0].read_text(), self.previous)
        protected.update({p: unchanged(p) for p in (self.target, self.backups()[0])})
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])
        self.calls.clear()
        self.invoke(update=True, dry_run=True, enable=True)
        self.assertEqual(self.mutations(), [])
        self.assertTrue(any(c[0].endswith('systemd-analyze') for c in self.calls))
        self.calls.clear()
        with self.assertRaisesRegex(RuntimeError, 'daemon-reload failed'):
            self.invoke(update=True, enable=True)
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])
        self.assertEqual({p: unchanged(p) for p in protected}, protected)
        self.calls.clear()
        self.invoke(update=True)
        self.assertEqual(self.props['NeedDaemonReload'], 'no')
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])
        self.assertTrue(any(c[0].endswith('systemd-analyze') for c in self.calls))
        self.calls.clear()
        self.invoke(update=True)
        self.assertEqual(self.mutations(), [])
        self.assertEqual(len(self.backups()), 1)
        self.assertEqual({p: unchanged(p) for p in protected}, protected)

    def test_active_reload_recovery_never_restarts_or_enables(self):
        self.existing(self.previous)
        self.props.update(ActiveState='active', MainPID=str(os.getpid()), UnitFileState='enabled')
        self.reload_failures = 1
        with patch.object(setup, 'gateway_pids', return_value=[os.getpid()]):
            with self.assertRaisesRegex(RuntimeError, 'daemon-reload failed'):
                self.invoke(update=True)
            self.calls.clear()
            self.invoke(update=True)
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])
        self.assertEqual(self.props['ActiveState'], 'active')
        self.assertEqual(self.props['MainPID'], str(os.getpid()))
        self.assertEqual(self.props['UnitFileState'], 'enabled')

    def test_pending_reload_does_not_bypass_conflict_checks(self):
        self.existing(self.rendered)
        self.props['NeedDaemonReload'] = 'yes'
        for key, value in (('User', 'another'), ('FragmentPath', '/other/unit'),
                           ('DropInPaths', '/other/dropin'), ('ActiveState', 'activating')):
            with self.subTest(key=key), patch.dict(self.props, {key: value}):
                with self.assertRaises(RuntimeError):
                    self.invoke(update=True)
        for value in (self.previous, self.rendered+'# administrator change\n'):
            self.target.write_text(value)
            with self.subTest(unit=value), self.assertRaises(RuntimeError):
                self.invoke(update=True)
            self.assertEqual(self.target.read_text(), value)
        self.assertEqual(self.mutations(), [])
        self.assertEqual(self.backups(), [])

    def test_admin_or_unknown_unit_never_overwritten(self):
        for value in (self.previous+'# administrator edit\n', self.previous.replace('RestartSec=3','RestartSec=4')):
            self.existing(value)
            with self.assertRaisesRegex(RuntimeError, 'Unrecognized'):
                self.invoke(update=True)
            self.assertEqual(self.target.read_text(), value)
        self.assertEqual(self.backups(), [])
        self.assertEqual(self.mutations(), [])

    def test_changed_user_or_executable_rejected(self):
        for value in (self.previous.replace('User='+self.account.pw_name, 'User=another'),
                      self.previous.replace(str(setup.EXECUTABLE), '/usr/bin/other')):
            self.existing(value)
            with self.assertRaisesRegex(RuntimeError, 'Unrecognized'):
                self.invoke(update=True)

    def test_new_template_cannot_change_protected_paths(self):
        for key in ('--state-dir /var/lib/kcf', '--lock-file /run/kcf/gateway.lock',
                    'StateDirectory=kcf', 'RuntimeDirectory=kcf'):
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, 'protected'):
                setup.approved_previous(self.previous, self.rendered.replace(key, key+'-new'), self.account)

    def test_declining_update_does_not_write(self):
        self.existing(self.previous)
        with patch.object(setup.sys.stdin, 'isatty', return_value=True), patch('builtins.input', return_value='no'):
            with self.assertRaisesRegex(RuntimeError, 'declined'):
                self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(self.backups(), [])

    def test_verify_failure_preserves_old_unit(self):
        self.existing(self.previous)
        original = setup.run
        def fail_verify(*args):
            if args[0].endswith('systemd-analyze'):
                raise RuntimeError('verify failed')
            return original(*args)
        with patch.object(setup, 'run', side_effect=fail_verify):
            with self.assertRaisesRegex(RuntimeError, 'verify failed'):
                self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(self.backups(), [])

    def test_active_service_is_not_restarted(self):
        self.existing(self.previous)
        self.props.update(ActiveState='active', MainPID=str(os.getpid()), UnitFileState='enabled')
        with patch.object(setup, 'gateway_pids', return_value=[os.getpid()]):
            self.invoke(update=True)
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload')])

    def test_start_requires_separate_enable(self):
        self.existing(self.previous)
        self.invoke(update=True, enable=True)
        self.assertEqual(self.mutations(), [('/usr/bin/systemctl', 'daemon-reload'),
                                           ('/usr/bin/systemctl', 'enable', '--now', setup.UNIT)])

    def test_backup_conflict_preserves_old_unit(self):
        self.existing(self.previous)
        backup = self.target.with_name(self.target.name+'.backup-'+hashlib.sha256(self.previous.encode()).hexdigest())
        backup.write_text('administrator data')
        backup.chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, 'backup conflicts'):
            self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(backup.read_text(), 'administrator data')

    def test_backup_failure_preserves_old_unit(self):
        self.existing(self.previous)
        with patch.object(setup.os, 'link', side_effect=PermissionError('backup refused')):
            with self.assertRaises(PermissionError):
                self.invoke(update=True)
        self.assertEqual(self.target.read_text(), self.previous)
        self.assertEqual(self.mutations(), [])

    def test_existing_dropin_and_unsafe_mode_rejected(self):
        self.existing(self.previous)
        self.props['DropInPaths'] = '/run/systemd/system/override.conf'
        with self.assertRaisesRegex(RuntimeError, 'drop-ins'):
            self.invoke(update=True)
        self.props['DropInPaths'] = ''
        self.target.chmod(0o666)
        with self.assertRaisesRegex(RuntimeError, 'Unsafe'):
            self.invoke(update=True)

    def test_nonroot_apply_refused(self):
        with patch.object(setup.os, 'geteuid', return_value=1000):
            with self.assertRaisesRegex(RuntimeError, 'requires sudo/root'):
                self.invoke(update=True)


class AccountTests(unittest.TestCase):
    """Exercise select_account itself; mock only the OS account/config inputs."""
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.account = pwd.struct_passwd(('kcfuser', 'x', 1000, 1000, '', '/home/kcfuser', '/bin/bash'))
        self.lookup = self.stack.enter_context(patch.object(setup.pwd, 'getpwnam', return_value=self.account))
        self.entries = self.stack.enter_context(patch.object(setup.pwd, 'getpwall', return_value=[self.account]))
        def config(path, *args, **kwargs):
            if str(path) == '/etc/login.defs':
                return 'UID_MIN 1000\n'
            if str(path) == '/etc/shells':
                return '/bin/bash\n/usr/sbin/nologin\n'
            self.fail(f'Unexpected config read: {path}')
        self.stack.enter_context(patch.object(Path, 'read_text', config))
        self.stack.enter_context(patch.dict(os.environ, {}, clear=True))
        self.stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def test_explicit_noninteractive_account(self):
        with patch('builtins.input', side_effect=AssertionError('Unexpected prompt')):
            self.assertEqual(setup.select_account('kcfuser', False), self.account)

    def test_noninteractive_requires_user_even_with_sudo_user(self):
        with patch.dict(os.environ, {'SUDO_USER': 'kcfuser'}):
            with self.assertRaisesRegex(RuntimeError, 'Non-interactive'):
                setup.select_account(None, False)
        self.lookup.assert_not_called()

    def test_interactive_sudo_and_explicit_user_require_confirmation(self):
        with patch.dict(os.environ, {'SUDO_USER': 'kcfuser'}):
            for name in (None, 'kcfuser'):
                with self.subTest(name=name), patch('builtins.input', return_value='yes') as prompt:
                    self.assertEqual(setup.select_account(name, True), self.account)
                    prompt.assert_called_once()
                with patch('builtins.input', return_value='no'):
                    with self.assertRaisesRegex(RuntimeError, 'declined'):
                        setup.select_account(name, True)

    def test_missing_and_unknown_account(self):
        with self.assertRaisesRegex(RuntimeError, 'unambiguous'):
            setup.select_account(None, True)
        self.lookup.side_effect = KeyError('missing')
        with self.assertRaisesRegex(RuntimeError, 'Unknown account'):
            setup.select_account('missing', False)

    def test_numeric_uid_and_unit_syntax_rejected(self):
        for name in ('0', '1000', '-1', 'kcfuser\nUser=root'):
            with self.subTest(name=name), self.assertRaisesRegex(RuntimeError, 'account name'):
                setup.select_account(name, False)
        self.lookup.assert_not_called()

    def test_root_system_uid_and_nonlogin_shell_rejected(self):
        for uid, shell in ((0, '/bin/bash'), (999, '/bin/bash'), (65534, '/bin/bash'),
                           (1000, '/usr/sbin/nologin'), (1000, '/unknown/shell')):
            self.lookup.return_value = pwd.struct_passwd(('kcfuser', 'x', uid, 1000, '', '/home/kcfuser', shell))
            with self.subTest(uid=uid, shell=shell), self.assertRaisesRegex(RuntimeError, 'non-root login'):
                setup.select_account('kcfuser', False)

    def test_duplicate_uid_alias_rejected(self):
        alias = pwd.struct_passwd(('alias', 'x', 1000, 1000, '', '/home/alias', '/bin/bash'))
        self.entries.return_value = [self.account, alias]
        with self.assertRaisesRegex(RuntimeError, 'Ambiguous'):
            setup.select_account('kcfuser', False)


if __name__ == '__main__':
    unittest.main()
