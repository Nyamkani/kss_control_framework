#!/usr/bin/env python3
"""Opt-in real virtual LAN integration; all privileged operations stay in userns.

The controller's stdin/stdout pipes carry test commands/observations only. KCF
payload transfer uses the actual veth NIC and UDP/TCP, never a pipe or mock.
"""
import json
import os
from pathlib import Path
import select
import signal
import subprocess as sp
import sys
import time
import traceback


def command(*args, **kwargs):
    return sp.run(list(args), check=True, text=True, stdout=sp.PIPE,
                  stderr=sp.PIPE, **kwargs).stdout


def keeper():
    command('mount', '--make-rprivate', '/')
    command('mount', '-t', 'tmpfs', '-o', 'size=64m,nosuid,nodev', 'kcf-test-shm', '/dev/shm')
    print('READY', flush=True)
    while True:
        signal.pause()


class Process:
    def __init__(self, lab, host, mode):
        self.lab, self.host, self.mode = lab, host, mode
        self.label = f'{host}-{mode}-{len(lab.processes)}'
        self.stderr = (lab.output / (self.label + '.stderr')).open('w')
        self.proc = sp.Popen(lab.enter(host) + [lab.binary, mode, str(host)],
                             stdin=sp.PIPE, stdout=sp.PIPE, stderr=self.stderr)
        lab.processes.append(self)
        self.ready = self.read()
        require(self.ready.get('ready') == 1, f'{self.label} startup: {self.ready}')

    def read(self, timeout=20):
        if not select.select([self.proc.stdout], [], [], timeout)[0]:
            raise AssertionError(f'{self.label}: controller wait exceeded {timeout}s')
        line = self.proc.stdout.readline()
        require(bool(line), f'{self.label} exited, see stderr (code={self.proc.poll()})')
        result = json.loads(line)
        self.lab.events.write(json.dumps({'time': time.monotonic(), 'process': self.label,
                                         'response': result}) + '\n')
        self.lab.events.flush()
        return result

    def ask(self, op='STATUS'):
        self.lab.events.write(json.dumps({'time': time.monotonic(), 'process': self.label,
                                         'command': op}) + '\n')
        self.proc.stdin.write((op + '\n').encode())
        self.proc.stdin.flush()
        return self.read()

    def stop(self, kill=False):
        if self.proc.poll() is None:
            if kill:
                self.proc.kill()
            else:
                try:
                    self.proc.stdin.write(b'QUIT\n'); self.proc.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
            try:
                self.proc.wait(timeout=5)
            except sp.TimeoutExpired:
                self.proc.kill(); self.proc.wait(timeout=5)
        self.stderr.close()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def until(predicate, message, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(message)


class Lab:
    def __init__(self, binary, output):
        self.binary, self.output = str(Path(binary).resolve()), Path(output).resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.events = (self.output/'events.jsonl').open('w')
        self.processes, self.keepers, self.results = [], {}, {}

    def enter(self, host):
        return ['nsenter', '-t', str(self.keepers[host].pid), '-n', '-i', '-m', '--']

    def inside(self, host, *args):
        return command(*(self.enter(host) + list(args)))

    def record(self, name, state='PASS', details=None):
        self.results[name] = {'status': state, 'details': details}
        print(f'{name}: {state}' + (f' {details}' if details else ''), flush=True)
        (self.output/'results.json').write_text(json.dumps(self.results, indent=2))

    def setup(self):
        command('mount', '--make-rprivate', '/')
        # The bridge lives in the outer private network namespace, without an
        # IP, uplink, NAT, default route or host firewall change.
        command('ip', 'link', 'add', 'kcfbr0', 'type', 'bridge', 'mcast_snooping', '0')
        command('ip', 'link', 'set', 'kcfbr0', 'up')
        for host in (1, 2):
            p = sp.Popen(['unshare', '--net', '--ipc', '--mount', sys.executable,
                          str(Path(__file__).resolve()), '--keeper'], stdout=sp.PIPE)
            self.keepers[host] = p
            require(select.select([p.stdout], [], [], 5)[0] and p.stdout.readline() == b'READY\n',
                    'Host keeper namespace/mount creation failed')
            outer, inner = f'veth{host}', f'lan{host}'
            command('ip', 'link', 'add', outer, 'type', 'veth', 'peer', 'name', inner)
            command('ip', 'link', 'set', inner, 'netns', str(p.pid))
            command('ip', 'link', 'set', outer, 'master', 'kcfbr0')
            command('ip', 'link', 'set', outer, 'up')
            self.inside(host, 'ip', 'link', 'set', inner, 'name', 'lan0')
            self.inside(host, 'ip', 'addr', 'add', f'10.203.0.{host+1}/24', 'dev', 'lan0')
            self.inside(host, 'ip', 'link', 'set', 'lo', 'up')
            self.inside(host, 'ip', 'link', 'set', 'lan0', 'up')
            self.inside(host, 'ip', 'route', 'add', '224.0.0.0/4', 'dev', 'lan0')
        topology = {str(h): {'pid': p.pid, 'namespaces': {
            k: os.readlink(f'/proc/{p.pid}/ns/{k}') for k in ('net', 'ipc', 'mnt')},
            'addresses': json.loads(self.inside(h, 'ip', '-j', 'addr')),
            'routes': json.loads(self.inside(h, 'ip', '-j', 'route'))} for h,p in self.keepers.items()}
        for ns in ('net', 'ipc', 'mnt'):
            require(topology['1']['namespaces'][ns] != topology['2']['namespaces'][ns], 'namespace not isolated')
        (self.output/'topology.json').write_text(json.dumps(topology, indent=2))
        self.record('namespace_isolation')

    def shm(self, host):
        return self.inside(host, 'python3', '-c', 'import os,json; print(json.dumps(sorted(os.listdir("/dev/shm"))))').strip()

    def netem(self, args, udp_only=False):
        # Egress of A only. prio band 1 selects UDP data port, leaving TCP and
        # Discovery on band 0. delay/jitter tests intentionally affect all IP.
        self.inside(1, 'tc', 'qdisc', 'del', 'dev', 'lan0', 'root') if self.qdisc else None
        self.qdisc = False
        if not args:
            return
        if udp_only:
            self.inside(1, 'tc', 'qdisc', 'add', 'dev', 'lan0', 'root', 'handle', '1:', 'prio',
                        'bands', '3', 'priomap', *(['0']*16))
            self.qdisc = True
            self.inside(1, 'tc', 'qdisc', 'add', 'dev', 'lan0', 'parent', '1:2', 'handle', '20:', 'netem', *args)
            self.inside(1, 'tc', 'filter', 'add', 'dev', 'lan0', 'protocol', 'ip', 'parent', '1:',
                        'prio', '1', 'u32', 'match', 'ip', 'protocol', '17', '0xff',
                        'match', 'ip', 'dport', '37652', '0xffff', 'flowid', '1:2')
        else:
            self.inside(1, 'tc', 'qdisc', 'add', 'dev', 'lan0', 'root', 'netem', *args)
            self.qdisc = True

    def cleanup(self):
        for p in reversed(self.processes):
            p.stop()
        for p in self.keepers.values():
            p.terminate()
            try: p.wait(timeout=3)
            except sp.TimeoutExpired: p.kill(); p.wait()
        self.events.close()

    def run(self):
        self.qdisc = False
        self.setup()
        a, b = Process(self, 1, 'owner'), Process(self, 2, 'owner')
        shm_a, shm_b = json.loads(self.shm(1)), json.loads(self.shm(2))
        require(any('virtual%2Fa' in n for n in shm_a) and not any('virtual%2Fb' in n for n in shm_a), 'A SHM contains B')
        require(any('virtual%2Fb' in n for n in shm_b) and not any('virtual%2Fa' in n for n in shm_b), 'B SHM contains A')
        require(not set(shm_a).intersection(shm_b), 'Local registry/Action/Service SHM visible across Hosts')
        for owner in (a,b):
            probe=owner.ask('ISOLATION');require(probe['parameter_open']==-2 and probe['topic_open']==-2,'cross-host Local open should be ENOENT')
        self.record('ipc_storage_isolation', details={'A': shm_a, 'B': shm_b})
        ga, gb = Process(self, 1, 'gateway'), Process(self, 2, 'gateway')
        def discovered():
            x, y = ga.ask(), gb.ask()
            return x['online'] and y['online'] and x['metadata'] and y['metadata'] and x['compatible'] and y['compatible']
        until(discovered, 'mutual multicast discovery failed')
        for gateway in (ga, gb):
            s = gateway.ask()
            require(s['virtual_ip'] and s['peers']==1 and not s['self'] and s['capabilities'] and s['action_endpoints'] and s['topic_endpoints'], 'discovery identity/metadata mismatch')
        self.record('discovery_v2_multicast', details={'A': ga.ask(), 'B': gb.ask()})
        time.sleep(.8)
        require(ga.ask().get('sent',0)==0 and gb.ask().get('received',0)==0, 'payload without subscriber')
        self.record('topic_no_subscriber_no_send')
        require(gb.ask('GET')['value']==10, 'remote get')
        require(not gb.ask('SET 123')['code'] and a.ask()['parameter']==123, 'remote set did not commit locally')
        require(gb.ask('PRIVATE')['code']!=0 and gb.ask('WRONG')['code']!=0, 'policy/type gate')
        require(ga.ask('GET')['value']==20, 'reverse parameter get')
        self.record('remote_parameter')
        def controls():
            require(not gb.ask('SET 124')['code'] and gb.ask('GET')['value']==124, 'parameter under load')
            r=gb.ask('SERVICE 21'); require(not r['code'] and r['value']==42, 'service response')
            r=gb.ask('ACTION 7'); require(not r['code'] and r['accepted'] and r['succeeded'] and r['value']==14 and r['feedback_updates']>=2, 'action lifecycle')
            r=gb.ask('CANCEL 8'); require(not r['code'] and not r['cancel_code'] and r['cancel_accepted'] and r['canceled'], 'cancel ack/completion')
            require(not a.ask()['worker_error'],'owner worker failed')
            return r
        until(lambda: gb.ask()['proxies']>=1, 'proxy bootstrap')
        sub=Process(self,2,'subscriber')
        until(lambda: sub.ask()['callbacks']>=5, 'local subscriber did not read remote proxy')
        t=time.monotonic(); before_a=a.ask(); before_g=ga.ask(); before_s=sub.ask(); time.sleep(2)
        after_a=a.ask(); after_g=ga.ask(); after_s=sub.ask(); elapsed=time.monotonic()-t
        local_hz=(after_a['published']-before_a['published'])/elapsed
        remote_hz=(after_g.get('sent',0)-before_g.get('sent',0))/elapsed
        require(local_hz>80 and 5<remote_hz<=55 and local_hz>2*remote_hz, 'rate sampling/local independence')
        require(after_s['callbacks']>before_s['callbacks'] and not after_s['bad'] and after_s['read']==0 and after_s['sequence']>before_s['sequence'], 'payload/type/sequence mismatch')
        a.ask('PAUSE'); time.sleep(.3); s=ga.ask().get('sent',0); time.sleep(.5)
        require(ga.ask().get('sent',0)==s, 'same Local sequence sent again'); a.ask('RESUME')
        self.record('remote_topic', details={'local_hz':local_hz,'remote_hz':remote_hz})
        before=sub.ask()['callbacks']; cancel=controls(); require(sub.ask()['callbacks']>before and a.ask()['service_calls']>0, 'topic/control stalled')
        self.record('remote_service'); self.record('remote_action',details=cancel); self.record('control_topic_simultaneous')
        # Real peer process death, while the original Local owner keeps running.
        old=gb.ask(); ga.stop(kill=True)
        until(lambda: not gb.ask()['online'] and gb.ask()['receive_routes']==0, 'peer kill did not become LOST')
        require(gb.ask('GET')['code']!=0, 'request to lost peer succeeded')
        before=a.ask(); time.sleep(.15); after=a.ask();require(after['local_read']==0 and after['published']>before['published'], 'Local IPC affected by gateway kill')
        self.record('peer_kill_local_survival')
        ga=Process(self,1,'gateway');until(discovered,'peer restart discovery')
        until(lambda: gb.ask()['receive_routes']==1 and sub.ask()['callbacks']>after_s['callbacks'], 'topic route restart')
        new=gb.ask();require(new['peer_session']!=old['peer_session'] and new['source_session']==new['peer_session'] and new['route_id']!=old['route_id'], 'stale route reused')
        controls();self.record('peer_restart',details={'old_session':old['peer_session'],'new_session':new['peer_session']})
        # Actual veth down, not a fake peer-table event.
        self.inside(1,'ip','link','set','lan0','down')
        until(lambda: not gb.ask()['online'] and gb.ask()['receive_routes']==0 and '10.203.0.2:' not in self.inside(2,'ss','-Htn','state','established'), 'link down isolation')
        require(gb.ask('GET')['code']!=0 and a.ask()['local_read']==0,'link down changed Local IPC')
        self.inside(1,'ip','link','set','lan0','up');until(discovered,'link up rediscovery')
        before=sub.ask()['callbacks'];until(lambda: sub.ask()['callbacks']>before,'topic after link up');controls();self.record('network_down_up')
        # Probe netem support explicitly. This namespace never changes host qdisc.
        try:
            self.netem(['loss','5%'],udp_only=True)
        except sp.CalledProcessError as e:
            self.record('udp_loss','LIMITATION',e.stderr)
            self.record('udp_loss_reorder','LIMITATION',e.stderr)
            if self.qdisc: self.netem([])
            self.record('delay_jitter_timeout','LIMITATION','netem unavailable')
        else:
            baseline=gb.ask();before=sub.ask()['callbacks'];time.sleep(6);r=gb.ask('GET');current=gb.ask()
            require(not r['code'] and sub.ask()['callbacks']>before and current.get('missed',0)>baseline.get('missed',0),'UDP loss not observed/next sample failed')
            self.record('udp_loss',details={'missed_delta':current.get('missed',0)-baseline.get('missed',0)})
            self.netem(['delay','70ms','10ms','loss','5%','duplicate','30%','reorder','50%','25%'],udp_only=True)
            baseline=gb.ask();before=sub.ask()['callbacks'];time.sleep(6);current=gb.ask()
            require(sub.ask()['callbacks']>before and current.get('duplicate',0)>baseline.get('duplicate',0) and current.get('out_of_order',0)>baseline.get('out_of_order',0),'netem duplicate/reorder not observed')
            (self.output/'netem-topic.txt').write_text(self.inside(1,'tc','-s','qdisc','show','dev','lan0'))
            self.record('udp_loss_reorder',details={k:current.get(k,0)-baseline.get(k,0) for k in ('missed','duplicate','out_of_order')})
            self.netem(['delay','40ms','10ms'])
            controls()
            calls=a.ask()['service_calls'];r=gb.ask('TIMEOUT_SERVICE')
            require(r['code']==-110 and r['execution_unknown'], 'service timeout semantics')
            time.sleep(1);require(a.ask()['service_calls']==calls+1,'service was automatically retried')
            goals=a.ask()['goals'];r=gb.ask('TIMEOUT_ACTION')
            require(r['code']==-110 and r['execution_unknown'], 'action timeout semantics')
            time.sleep(2.5);require(a.ask()['goals']==goals+1,'action was automatically retried')
            self.record('delay_jitter_timeout',details='40±10 ms A egress; dispatched slow operation timeout is execution_unknown, no retry')
            self.netem([])
        # Receiver Gateway SIGKILL leaves a proven Network proxy. New Gateway
        # reclaims it; existing Local reader mappings still reconnect explicitly.
        old=gb.ask();gb.stop(kill=True);objects=json.loads(self.shm(2))
        require('virtual%2Fa%2Ftopic' in objects,'SIGKILL did not leave proxy')
        require(any(n.startswith('kcf_network_proxy_') for n in objects),'ownership sidecar missing')
        until(lambda:not ga.ask()['online'],'receiver kill not lost')
        sub.stop()
        gb=Process(self,2,'gateway');until(discovered,'receiver restart discovery')
        until(lambda:gb.ask().get('proxy_recovered',0)==1 and gb.ask().get('proxy_stale_detected',0)==1,'automatic stale proxy recovery missing')
        sub=Process(self,2,'subscriber');until(lambda:sub.ask()['callbacks']>=5,'subscriber reconnect after automatic recovery failed');controls()
        now=gb.ask();require(now['destination_session']!=old['destination_session'],'old local Gateway session reused')
        self.record('gateway_shm_auto_recovery',details={'retained_after_kill':objects,'gateway':now,'manual_unlink':False,'subscriber_recreated':True})
        # Normal Gateway destruction cleans its proven owned names. Same-object
        # Stop/Start intentionally retains mappings; this tests process exit.
        sub.stop();gb.stop()
        require('virtual%2Fa%2Ftopic' not in json.loads(self.shm(2)),'normal Gateway exit did not release proxy')
        gb=Process(self,2,'gateway');until(discovered,'normal Gateway restart discovery')
        until(lambda:gb.ask()['proxies']>=1,'normal proxy recreate')
        sub=Process(self,2,'subscriber');until(lambda:sub.ask()['callbacks']>=5,'normal restart subscriber failed');controls()
        self.record('gateway_normal_restart')
        self.record('physical_lan','LIMITATION','Virtual veth/bridge only; same Linux kernel/CPU, no physical NIC/switch/Wi-Fi')


def main():
    if sys.argv[1:] == ['--keeper']:
        keeper();return 0
    if len(sys.argv)!=4 or sys.argv[1]!='--run':return 2
    lab=Lab(sys.argv[2],sys.argv[3]);code=0
    try: lab.run()
    except Exception as e:
        code=1;lab.record('execution','FAIL',str(e));(lab.output/'failure.txt').write_text(traceback.format_exc());traceback.print_exc()
    finally:
        lab.cleanup()
    lab.record('cleanup',details='children reaped; keeper namespaces/tmpfs released; outer PID/net namespace removed on runner exit')
    for name in ('namespace_isolation','ipc_storage_isolation','discovery_v2_multicast','remote_parameter','remote_service','remote_action','remote_topic','control_topic_simultaneous','peer_kill_local_survival','peer_restart','network_down_up','udp_loss','udp_loss_reorder','delay_jitter_timeout','gateway_shm_auto_recovery','gateway_normal_restart'):
        if name not in lab.results: lab.record(name,'LIMITATION','Not executed after an earlier failure')
    lab.record('overall','FAIL' if code else 'PASS','Physical LAN remains unverified; old Local reader mappings reconnect explicitly')
    return code

if __name__=='__main__':sys.exit(main())
