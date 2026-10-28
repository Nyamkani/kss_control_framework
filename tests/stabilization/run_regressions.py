#!/usr/bin/env python3
"""Serial stabilization runner; no product/test source changes or host IPC cleanup.

Example (from the Framework root):
  python3 tests/stabilization/run_regressions.py --framework-root . \
    --tools-root ../kcf_tools --output-dir /tmp/kcf-smoke --mode smoke \
    --qt-prefix /opt/Qt/6.9.3/gcc_64

Outputs must be new/empty: first results are never overwritten. Default builds
live under OUTPUT/build/{framework,tools}; explicit build overrides must belong
 to the requested source tree and use Debug. Stress repeats Tools core tests 10
 times by default. --diagnostic-reruns is opt-in and never changes a first FAIL.
E2E covers isolated virtual networks only, never physical LAN or systemd.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess as sp
import sys
import time

KNOWN = [
    'Intermittent Service endpoint selection -ENOENT: cause unconfirmed.',
    'Historical Integration/ControlTraffic first failures remain; a later PASS does not erase them.',
    'Type Registry EAGAIN recovery stops after 64 attempts; Service/Action registries are outside that recovery scope.',
]
UNVERIFIED = ['Physical LAN', 'Actual systemd operation', 'Hard real-time / guaranteed 100 Hz',
              'Robot fail-safe / PREEMPT_RT hardware']


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def git(root):
    def query(*args):
        return sp.check_output(['git', '-C', str(root), *args], text=True, timeout=15).strip()
    status = query('status', '--short')
    return dict(root=str(root), head=query('rev-parse', 'HEAD'),
                branch=query('branch', '--show-current'), dirty=bool(status), status=status)


def cache(build):
    f = build / 'CMakeCache.txt'
    if not f.exists():
        return {}
    return dict((m.group(1), m.group(2)) for line in f.read_text().splitlines()
                if (m := re.match(r'([^/#][^:]*):[^=]+=(.*)', line)))


class Runner:
    def __init__(self, args):
        self.a = args
        self.out = args.output_dir
        self.out.mkdir(parents=True, exist_ok=True)
        if any(self.out.iterdir()):
            raise ValueError('output-dir must be empty; select a new directory to preserve earlier results')
        self.logs = self.out / 'logs'
        self.logs.mkdir()
        self.records = []
        self.complete = False
        self.context = dict(started_at=now(), mode=args.mode, build_type='Debug',
                            environment=dict(kernel=platform.release(), platform=platform.platform(),
                                             python=sys.version, uid=os.getuid(), cpu_count=os.cpu_count(),
                                             affinity=sorted(os.sched_getaffinity(0)),
                                             qt='offscreen', execution='serial isolated Linux namespaces'),
                            git={'framework': git(args.framework_root), 'tools': git(args.tools_root)},
                            configuration={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()})
        self.ns = False
        self.save()

    def save(self):
        status = 'FAIL' if any(r['status'] == 'FAIL' for r in self.records) else (
            'SKIP' if any(r['status'] == 'SKIP' for r in self.records) else 'PASS')
        if not self.complete:
            status = 'IN_PROGRESS'
        report = dict(context=self.context, complete=self.complete, overall=status,
                      known_issues=KNOWN, unverified=UNVERIFIED, results=self.records)
        temp = self.out / 'results.json.tmp'
        temp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        temp.replace(self.out / 'results.json')
        lines = [f'# {self.a.mode} regression result: {status}', '',
                 f"Started: {self.context['started_at']} (UTC); build: Debug; serial execution.", '',
                 '## First failures and diagnostic reruns', '']
        failed = [r for r in self.records if r['status'] == 'FAIL']
        lines += [f"- {r['name']} iteration {r['iteration']}, attempt {r['attempt']}: "
                  f"exit={r['exit_code']}, {r['reason']} ([stderr]({r['stderr']}))" for r in failed] or ['None recorded.']
        reruns = [r for r in self.records if r['attempt'] > 1]
        lines += [f"- Diagnostic rerun: {r['name']} #{r['iteration']}, attempt {r['attempt']}: {r['status']}" for r in reruns]
        lines += ['', '## Results', '', '| Test / stage | Iteration | Attempt | Result | Seconds | Logs |',
                  '|---|---:|---:|---|---:|---|']
        for r in self.records:
            lines.append(f"| {r['name']} | {r['iteration']} | {r['attempt']} | {r['status']} | {r['seconds']:.3f} | "
                         f"[out]({r['stdout']}) / [err]({r['stderr']}) |")
        lines += ['', '## Environment SKIP', '']
        lines += [f"- {r['name']}: {r['reason']}" for r in self.records if r['status'] == 'SKIP'] or ['None.']
        lines += ['', '## Known issues', ''] + ['- ' + x for x in KNOWN]
        lines += ['', '## Not verified / not certified', ''] + ['- ' + x for x in UNVERIFIED]
        lines += ['', 'Git HEAD, dirty state, full commands, timestamps and exit codes: `results.json`.',
                  'Earlier failures are retained, including when a diagnostic rerun passes.',
                  'This entry point reuses existing tests; it does not replace their coverage.', '']
        (self.out / 'RESULT.md').write_text('\n'.join(lines))

    @staticmethod
    def isolated(command):
        return ['unshare', '--user', '--map-root-user', '--net', '--ipc', '--mount', '--pid',
                '--fork', '--kill-child=KILL', '--mount-proc', 'sh', '-c',
                'ip link set lo up && mount -t tmpfs tmpfs /dev/shm && exec "$@"', 'sh', *command]

    @staticmethod
    def cleanup(process):
        # Only the session/process group created by this runner, never process-name matching.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=2)
        except sp.TimeoutExpired:
            pass
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()

    def run(self, name, command, *, iteration=1, attempt=1, isolate=True,
            timeout=None, skip=None, prerequisite=False):
        command = list(map(str, command))
        executed = self.isolated(command) if isolate else command
        index = len(self.records) + 1
        stem = f'{index:04d}-{re.sub(r"[^a-zA-Z0-9_.-]", "_", name)}-{iteration:02d}-{attempt}'
        stdout = self.logs / (stem + '.stdout.log')
        stderr = self.logs / (stem + '.stderr.log')
        config = self.out / 'settings' / stem
        config.mkdir(parents=True, mode=0o700)
        runtime = config / 'runtime'
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(config),
                   XDG_RUNTIME_DIR=str(runtime), PYTHONDONTWRITEBYTECODE='1', CTEST_PARALLEL_LEVEL='1')
        started, tick = now(), time.monotonic()
        code, reason, status = None, skip or '', 'SKIP' if skip else 'FAIL'
        interrupted = False
        with stdout.open('w') as out, stderr.open('w') as err:
            if isolate and not self.ns and not skip:
                skip = reason = 'isolated namespace preflight unavailable; no host fallback'
                status = 'SKIP'
            if skip:
                err.write(reason + '\n')
            else:
                process = None
                try:
                    process = sp.Popen(executed, stdout=out, stderr=err, env=env,
                                       cwd=self.a.framework_root, start_new_session=True)
                    code = process.wait(timeout=timeout or self.a.timeout)
                    status = 'PASS' if code == 0 else ('SKIP' if code == 77 or prerequisite else 'FAIL')
                    if code:
                        reason = f'exit {code}' + (' (environment prerequisite unavailable)' if prerequisite else '')
                except sp.TimeoutExpired:
                    reason = f'runner timeout {timeout or self.a.timeout}s (not increased)'
                except (KeyboardInterrupt, SystemExit):
                    reason, interrupted = 'interrupted; incomplete execution', True
                except OSError as e:
                    reason = str(e)
                    if prerequisite:
                        status = 'SKIP'
                finally:
                    if process:
                        self.cleanup(process)
                        if code is None:
                            code = process.returncode
        record = dict(name=name, stage=name, iteration=iteration, attempt=attempt, status=status,
                      reason=reason, command=command, executed_command=executed,
                      shell_command=shlex.join(executed), started_at=started, ended_at=now(),
                      seconds=time.monotonic()-tick, exit_code=code, timeout=timeout or self.a.timeout,
                      stdout=str(stdout.relative_to(self.out)), stderr=str(stderr.relative_to(self.out)),
                      git=self.context['git'], build_type='Debug', environment=self.context['environment'],
                      xdg_config_home=str(config))
        self.records.append(record)
        self.save()
        print(f"{status} {name} #{iteration} attempt={attempt} exit={code} {record['seconds']:.2f}s {reason}", flush=True)
        if interrupted:
            raise KeyboardInterrupt
        return record

    def test(self, name, command, **kwargs):
        result = self.run(name, command, **kwargs)
        if result['status'] == 'FAIL':
            for attempt in range(2, self.a.diagnostic_reruns + 2):
                self.run(name, command, attempt=attempt, **kwargs)
        return result

    def prepare(self):
        missing = [s for s in ['unshare', 'ip', 'mount', 'sh'] if not shutil.which(s)]
        r = self.run('namespace-preflight', self.isolated(['true']), isolate=False, timeout=20,
                     prerequisite=True, skip='missing tools: ' + ', '.join(missing) if missing else None)
        self.ns = r['status'] == 'PASS'
        ok = {}
        for repo, build in [('framework', self.a.framework_build), ('tools', self.a.tools_build)]:
            root = getattr(self.a, repo + '_root')
            build.mkdir(parents=True, exist_ok=True)
            values = cache(build)
            if values and (Path(values.get('CMAKE_HOME_DIRECTORY', '')).resolve() != root or
                           values.get('CMAKE_BUILD_TYPE') != 'Debug' or
                           (repo == 'tools' and Path(values.get('KCF_SOURCE_DIR', '')).resolve() != self.a.framework_root)):
                raise ValueError(f'refusing foreign/non-Debug build cache: {build}')
            cmd = ['cmake', '-S', root, '-B', build, '-DCMAKE_BUILD_TYPE=Debug']
            if repo == 'tools':
                cmd += [f'-DKCF_SOURCE_DIR={self.a.framework_root}', '-DKCF_TOOL_BUILD_TESTS=ON', '-DKCF_TOOL_BUILD_GUI=ON']
                if self.a.qt_prefix:
                    cmd += [f'-DCMAKE_PREFIX_PATH={self.a.qt_prefix}']
            configured = self.run(repo + '-configure', cmd, isolate=False, timeout=self.a.build_timeout)
            built = self.run(repo + '-build', ['cmake', '--build', build, '-j', str(self.a.jobs)],
                             isolate=False, timeout=self.a.build_timeout,
                             skip='configure failed' if configured['status'] != 'PASS' else None)
            ok[repo] = built['status'] == 'PASS'
        self.ready = ok
        self.ctests = []
        if ok['tools']:
            r = self.run('tools-ctest-discovery', ['ctest', '--test-dir', self.a.tools_build, '--show-only=json-v1'], isolate=False)
            if r['status'] == 'PASS':
                self.ctests = [t['name'] for t in json.loads((self.out/r['stdout']).read_text())['tests']]
                if not self.ctests:
                    raise ValueError('Tools CTest inventory is empty')

    def framework(self, name, path, args=(), iteration=1):
        return self.test(name, [self.a.framework_build / path, *args], iteration=iteration,
                         skip=None if self.ready['framework'] else 'Framework build failed')

    def tool_test(self, name, iteration=1):
        reason = None if name in self.ctests else 'CTest not registered / Tools build unavailable'
        return self.test(name, ['ctest', '--test-dir', self.a.tools_build, '-j1', '--output-on-failure',
                               '--no-tests=error', '-R', '^' + re.escape(name) + '$'],
                         iteration=iteration, skip=reason)

    def smoke(self):
        f = self.a.framework_build
        for name in ['type_registration_recovery', 'dynamic_access', 'endpoint_registry']:
            self.framework(name, f'examples/introspection/kcf_{name}_test')
        self.framework('topic-full', 'examples/network/kcf_topic_data_test')
        self.framework('topic-conflict', 'examples/network/kcf_topic_data_test', ['--case', 'conflict'])
        for name, args in [('gateway', []), ('gateway_lifecycle', []), ('gateway_topic_policy', ['--network']), ('gateway_local_ipc', [])]:
            self.framework(name, f'gateway/kcf_{name}_test', args)
        self.framework('element-name', 'examples/supervisor/kcf_element_name_test', [f/'bringup/kcf_bringup'])
        self.framework('supervisor-failure-contract', 'examples/supervisor/kcf_supervisor_failure_contract_test',
                       [f/'examples/supervisor/kcf_supervisor_runtime_test'])
        if not self.ctests:
            self.run('tools-all-ctest', [], skip='Tools CTest inventory unavailable')
        for name in self.ctests:
            self.tool_test(name)
        self.test('tools-element-name-graph', [self.a.tools_build/'kcf_tool_element_name_graph',
                  f/'bringup/kcf_bringup', f/'examples/supervisor/kcf_element_name_test'],
                  skip=None if all(self.ready.values()) else 'build unavailable')
        self.test('tools-local-gateway-real', [self.a.tools_build/'kcf_tool_local_gateway_real', f/'gateway/kcf_gateway'],
                  skip=None if all(self.ready.values()) else 'build unavailable')

    def stress(self):
        for name, count, path, args in [
            ('conflict', 50, 'network/kcf_topic_data_test', ['--case', 'conflict']),
            ('topic-full', 20, 'network/kcf_topic_data_test', []),
            ('dynamic-access', 30, 'introspection/kcf_dynamic_access_test', ['--case', 'topic-concurrent']),
            ('type-recovery', 20, 'introspection/kcf_type_registration_recovery_test', []),
            ('service', 30, 'network/kcf_topic_data_test', ['--case', 'control-traffic'])]:
            for i in range(1, count + 1):
                self.framework(name, 'examples/' + path, args, i)
        names = ['kcf_tool_network_backend', 'kcf_tool_network_graph', 'kcf_tool_network_diagnostics',
                 'kcf_tool_graph_model', 'kcf_tool_topic_observer', 'kcf_tool_local_gateway', 'kcf_tool_local_gateway_ui']
        for i in range(1, self.a.tools_repetitions + 1):
            for name in names:
                self.tool_test(name, i)

    def e2e(self):
        required = ['unshare', 'nsenter', 'ip', 'tc', 'ss', 'mount']
        missing = [s for s in required if not shutil.which(s)]
        probe = self.run('virtual-lan-preflight', ['sh', '-c',
            'ip link add probe0 type veth peer name probe1 && ip link add probe2 type dummy && '
            'tc qdisc add dev probe0 root netem delay 1ms'], prerequisite=True,
            skip='missing: ' + ', '.join(missing) if missing else None)
        reason = None if probe['status'] == 'PASS' else 'Virtual LAN namespace/veth/dummy/netem prerequisite unavailable'
        if not all(self.ready.values()):
            reason = 'build unavailable'
        f, t, b = self.a.framework_root, self.a.tools_root, self.a.tools_build
        # These orchestrators already manage their own namespaces, except remote_control_e2e.
        self.test('framework-virtual-lan', ['bash', f/'tests/network_virtual/run_virtual_network_test.sh',
                  self.a.framework_build, self.out/'virtual-framework'], isolate=False, timeout=600, skip=reason)
        self.test('tools-network-e2e', [sys.executable, t/'tests/network_e2e.py', f, b, self.out/'virtual-tools'],
                  isolate=False, timeout=300, skip=reason)
        self.test('tools-remote-control-e2e', [sys.executable, t/'tests/remote_control_e2e.py', f, b,
                  self.out/'virtual-control'], timeout=300, skip=reason)
        self.test('gateway-network-lifecycle', ['bash', f/'gateway/src/gateway_network_test.sh',
                  self.a.framework_build/'gateway'], isolate=False, timeout=300, skip=reason)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['framework-root', 'tools-root', 'output-dir']:
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--mode', choices=['smoke', 'stress', 'e2e'], required=True)
    parser.add_argument('--framework-build', type=Path)
    parser.add_argument('--tools-build', type=Path)
    parser.add_argument('--qt-prefix', type=Path)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--build-timeout', type=int, default=1800)
    parser.add_argument('--tools-repetitions', type=int, default=10)
    parser.add_argument('--diagnostic-reruns', type=int, default=0)
    args = parser.parse_args()
    for name in ['framework_root', 'tools_root', 'output_dir']:
        setattr(args, name, getattr(args, name).resolve())
    for name, repo in [('framework_build', 'framework'), ('tools_build', 'tools')]:
        path = getattr(args, name) or args.output_dir / 'build' / repo
        setattr(args, name, path.resolve())
    if min(args.jobs, args.timeout, args.build_timeout, args.tools_repetitions) <= 0 or args.diagnostic_reruns < 0:
        parser.error('counts and timeouts must be positive; reruns must be nonnegative')
    for root in [args.framework_root, args.tools_root]:
        if not (root/'CMakeLists.txt').is_file():
            parser.error(f'not a repository source root: {root}')
    for build in [args.framework_build, args.tools_build]:
        if build in [args.framework_root, args.tools_root] or any(build in r.parents for r in [args.framework_root, args.tools_root]):
            parser.error('build directory must not be a source root or its ancestor')
    runner = None
    try:
        runner = Runner(args)
        runner.prepare()
        getattr(runner, args.mode)()
        runner.complete = True
        runner.context['finished_at'] = now()
        runner.context['git_final'] = {'framework': git(args.framework_root), 'tools': git(args.tools_root)}
        runner.save()
        return 1 if any(r['status'] == 'FAIL' for r in runner.records) else (77 if any(r['status'] == 'SKIP' for r in runner.records) else 0)
    except (KeyboardInterrupt, Exception) as e:
        if runner:
            runner.context['incomplete_reason'] = str(e) or 'interrupted'
            runner.save()
        print(f'Incomplete: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    sys.exit(main())
