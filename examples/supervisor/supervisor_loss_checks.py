#!/usr/bin/env python3
"""Linux subreaper test: real Supervisor loss, plus raw protocol edge cases."""
import ctypes
import errno
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess as sp
import sys
import tempfile
import time

build=Path(sys.argv[1] if len(sys.argv)>1 else 'build').resolve()
runtime=str(build/'examples/supervisor/kcf_supervisor_runtime_test')
bringup=str(build/'bringup/kcf_bringup')
probe=str(build/'examples/supervisor/kcf_runtime_supervision_test')
assert ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0)==0  # PR_SET_CHILD_SUBREAPER

def until(fn,seconds=7):
    deadline=time.monotonic()+seconds
    while not fn():
        assert time.monotonic()<deadline, 'test deadline exceeded'
        time.sleep(.01)

def read(log):
    log.seek(0);return log.read()

def packet(i,kind=0):
    values=[0x4b435253,1,1,i]
    if kind:values[kind-1]=0 if kind!=3 else 2
    return struct.pack('<IHHQ',*values)

# Direct execution stays alive beyond the supervised timeout without a worker.
with tempfile.TemporaryFile(mode='w+') as log:
    p=sp.Popen([runtime,'--frequency','1000'],stdout=log,stderr=sp.STDOUT)
    try:
        time.sleep(2.5);assert p.poll() is None
        assert len(list(Path(f'/proc/{p.pid}/task').iterdir()))==1
        assert not any(os.readlink(f).startswith('socket:') for f in Path(f'/proc/{p.pid}/fd').iterdir())
    finally:p.terminate();p.wait(timeout=4)
    assert p.returncode==0 and 'result=0' in read(log),read(log)
print('Standalone >2s, one thread, no supervision socket PASS',flush=True)

for mode in ('silence','invalid','valid','expected-close','setup-loss','setup-timeout','setup-stop','loop-loss'):
    parent,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
    parent.setblocking(False)
    env=os.environ.copy();env['KCF_SUPERVISION_FD']=str(child.fileno())
    args=[runtime]
    if mode.startswith('setup-'):args+=['--setup-delay-ms','2800']
    if mode=='expected-close':args+=['--shutdown-delay-ms','2400']
    if mode=='loop-loss':args+=['--stall-after-loops','1','--stall-ms','2800']
    with tempfile.TemporaryFile(mode='w+') as log:
        p=sp.Popen(args,env=env,pass_fds=[child.fileno()],stdout=log,stderr=sp.STDOUT)
        child.close();start=time.monotonic()
        try:
            if mode in ('setup-loss','setup-stop','loop-loss','expected-close'):
                until(lambda:'Setup begin' in read(log))
                if mode=='loop-loss':until(lambda:'stall begin' in read(log))
                if mode in ('setup-stop','expected-close'):p.terminate();time.sleep(.05)
                parent.close()
            if mode=='valid':
                i=1
                while time.monotonic()-start<2.6:
                    assert p.poll() is None,read(log)
                    parent.send(packet(i));i+=1
                    time.sleep(.05)
                    try:parent.recv(4096)
                    except BlockingIOError:pass
                p.terminate()
            if mode=='invalid':
                i=1
                while p.poll() is None and time.monotonic()-start<3.5:
                    try:parent.send(packet(i,1+(i%4)))
                    except (BrokenPipeError,ConnectionResetError):break
                    i+=1;time.sleep(.025)
            p.wait(timeout=5)
            expected=0 if mode in ('valid','setup-stop','expected-close') else -errno.ECONNRESET if mode in ('setup-loss','loop-loss') else -errno.ETIMEDOUT
            out=read(log)
            assert p.returncode==(expected&255),out
            assert f'result={expected} ' in out and 'Shutdown loops=' in out,out
            if mode.startswith('setup-'):assert 'Shutdown loops=0' in out,out
            if mode in ('silence','invalid'):assert 1.8<time.monotonic()-start<3.5
            print(f'{mode}: main Shutdown, result={expected} PASS',flush=True)
        finally:
            parent.close()
            if p.poll() is None:p.kill();p.wait()

for mode in ('normal','crash','hang','setup-crash','safe-crash','safe-hang'):
    with tempfile.TemporaryFile(mode='w+') as log:
        args=[]
        for name in ('A','B','C'):
            if args:args+=['--next']
            executable=str(build/'examples/supervisor/kcf_supervisor_safe') if mode.startswith('safe-') and name=='C' else runtime
            args += [executable,'--element-name',name]
            if mode=='setup-crash':args+=['--setup-delay-ms','2800']
        p=sp.Popen([bringup,*args],stdout=log,stderr=sp.STDOUT)
        start_ticks=Path(f"/proc/{p.pid}/stat").read_text().rsplit(")",1)[1].split()[19]
        children=[]
        completed=False
        try:
            until(lambda:('state=1' if mode=='setup-crash' else 'INITIALIZING -> RUNNING') in read(log))
            children=[int(v) for v in Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().split()]
            assert len(children)==3
            if mode=='normal':
                time.sleep(2.5);assert p.poll() is None
                for pid in children:assert len(list(Path(f'/proc/{pid}/task').iterdir()))==2
                p.terminate();p.wait(timeout=5);assert p.returncode==0,read(log)
            elif mode in ('hang','safe-hang'):
                os.kill(p.pid,signal.SIGSTOP)
                until(lambda:read(log).count('result=-110 ')==(2 if mode=='safe-hang' else 3) and (mode!='safe-hang' or 'Shutdown safe=' in read(log)))
                os.kill(p.pid,signal.SIGCONT);time.sleep(.3)
                p.terminate();p.wait(timeout=5)
                assert p.returncode==1,read(log)
            else:
                p.kill();p.wait(timeout=4)
                for pid in children:
                    status=[]
                    def exited():
                        reaped,value=os.waitpid(pid,os.WNOHANG)
                        if reaped:status.append(value)
                        return bool(reaped)
                    until(exited)
                    assert os.WIFEXITED(status[0]) and os.WEXITSTATUS(status[0])==((-errno.ECONNRESET)&255),read(log)
                sp.run([probe,'--recover-system-status',str(p.pid),start_ticks],check=True,timeout=3)
            out=read(log)
            assert out.count('Shutdown loops=')==(2 if mode.startswith('safe-') else 3),out
            if mode.startswith('safe-'):assert 'Shutdown safe=' in out and 'output=0 error=0' in out,out
            if mode=='setup-crash':assert out.count('Shutdown loops=0')==3 and 'INITIALIZING -> RUNNING' not in out,out
            if mode=='normal':assert out.count('result=0 ')==3,out
            for pid in children:assert not Path(f'/proc/{pid}').exists()
            assert not Path(f'/dev/shm/kcf%2Fsystem%2Fstatus%2Fstate_{p.pid}_{start_ticks}').exists()
            completed=True
            print(f'Real Supervisor {mode}: A/B/C exit/reap PASS',flush=True)
        finally:
            if p.poll() is None:
                os.kill(p.pid,signal.SIGCONT);p.terminate();p.wait(timeout=6)
            # Failure cleanup only; never used for the successful crash assertion.
            for pid in ([] if completed else children):
                try:os.kill(pid,signal.SIGKILL)
                except ProcessLookupError:pass
                try:os.waitpid(pid,0)
                except ChildProcessError:pass
print('Supervisor loss checks PASS',flush=True)
