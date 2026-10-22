#!/usr/bin/env python3
import argparse,ctypes,json,os,pty,select,shlex,signal,subprocess,sys,termios,threading,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'application/mecanum/tests/integration'))
from run_motion import MotionUarts

def main():
    p=argparse.ArgumentParser()
    for x in ['build-dir','probe','watch','output-dir']:p.add_argument('--'+x,required=True)
    a=p.parse_args();out=Path(a.output_dir);out.mkdir(parents=True,exist_ok=True)
    executable=Path(a.build_dir)/'application/mecanum/keyboard/kcf_mecanum_keyboard'; results=[]
    def wait(fn,timeout=8):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            value=fn()
            if value:return value
            time.sleep(.005)
        raise AssertionError('condition timeout')
    for scenario in ['keys_escape','sigint','sigterm','sigkill']:
        prefix='/keyboard_test_'+str(os.getpid())+'_'+scenario;u=MotionUarts();procs=[];handles=[];fds=[];samples=[]
        try:
            def logproc(command,stdin=None):
                f=open(out/(scenario+'_'+str(len(procs))+'.log'),'w');handles.append(f)
                proc=subprocess.Popen(command,stdin=stdin,stdout=f,stderr=subprocess.STDOUT);procs.append(proc);return proc
            group=logproc([sys.executable,str(ROOT/'application/mecanum/bringup/run.py'),'--build-dir',a.build_dir,
                    '--motor-serial',u.paths[0],'--imu-serial',u.paths[1],'--topic-prefix',prefix,'--application-name',scenario])
            time.sleep(3.2);assert group.poll() is None
            def keyboard():
                master,slave=pty.openpty();fds.extend([master,slave]);saved=termios.tcgetattr(slave)
                proc=logproc([str(executable),'--linear-speed','0.12','--angular-speed','0.4','--topic-prefix',prefix],slave)
                return proc,master,slave,saved
            k,master,slave,saved=keyboard()
            watch=subprocess.Popen([a.watch,prefix],stdout=subprocess.PIPE,text=True);procs.append(watch)
            def consume():
                for line in watch.stdout:
                    v=line.split();samples.append({'sequence':int(v[0]),'generated_ns':int(v[1]),'v':list(map(float,v[2:]))})
            t=threading.Thread(target=consume,daemon=True);t.start();wait(lambda:len(samples)>3)
            # Actual runtime metadata: the keyboard must be STANDALONE.
            raw=subprocess.check_output([a.probe,prefix],text=True)
            runtimes=[shlex.split(x) for x in raw.splitlines() if x.startswith('R ')]
            assert any(int(x[1])==k.pid and x[-1]=='STANDALONE' for x in runtimes),runtimes
            def press(key,duration=.4):
                until=time.monotonic()+duration;last_written=0
                while time.monotonic()<until:
                    os.write(master,key.encode());last_written=time.monotonic_ns();time.sleep(.06)
                return last_written
            def events(after,zero=False):
                with u.lock:return [x for x in u.events if x['received_ns']>=after and (not any(x['rpm_uart']) if zero else any(x['rpm_uart']))]
            if scenario=='keys_escape':
                duplicate,dm,ds,original=keyboard()
                assert duplicate.wait(timeout=3)!=0 and k.poll() is None
                assert termios.tcgetattr(ds)==original
                results.append({'duplicate_publisher_rejected':True})
                cases=[('w',[1,0,0],[-1,-1,-1,-1]),('s',[-1,0,0],[1,1,1,1]),('a',[0,1,0],[1,-1,1,-1]),
                       ('d',[0,-1,0],[-1,1,-1,1]),('q',[0,0,1],[1,1,-1,-1]),('e',[0,0,-1],[-1,-1,1,1])]
                for key,direction,signs in cases:
                    start=time.monotonic_ns();press(key)
                    cmd=wait(lambda:next((x for x in reversed(samples) if x['generated_ns']>=start and any(x['v'])),None))
                    assert all(v*d>0 if d else v==0 for v,d in zip(cmd['v'],direction)),cmd
                    motion=wait(lambda:events(start));assert all(v*s>0 for v,s in zip(motion[-1]['rpm_uart'],signs))
                    stop_at=time.monotonic_ns();os.write(master,b' ')
                    zero=wait(lambda:events(stop_at,True))
                    results.append({'key':key,'cmd':cmd['v'],'rpm_uart':motion[-1]['rpm_uart'],'space_stop_ms':(zero[0]['received_ns']-stop_at)/1e6})
                    assert (zero[0]['received_ns']-stop_at)<250000000
                last_key=press('w')
                zero=wait(lambda:events(last_key+100000000,True))
                idle_delay=(zero[0]['received_ns']-last_key)/1e6;assert 140<idle_delay<400,idle_delay
                results.append({'input_idle_stop_ms':idle_delay})
                press('w');before=time.monotonic_ns();os.write(master,b'\x1b');assert k.wait(timeout=3)==0
                wait(lambda:samples[-1]['v']==[0,0,0]);wait(lambda:events(before,True))
                assert termios.tcgetattr(slave)==saved
                assert not Path('/dev/shm/'+(prefix+'/cmd_vel')[1:].replace('/','%2F')).exists()
                # Retained Motor subscriber stays attached to the old, unlinked object.
                k2,master,slave,saved=keyboard();time.sleep(.2);start=time.monotonic_ns();press('w',.7)
                assert k2.poll() is None
                assert not events(start),events(start)
                current=subprocess.check_output([a.probe,prefix],text=True)
                assert any(shlex.split(x)[0:3]==['F',prefix+'/cmd_vel','vx_m_s'] and float(shlex.split(x)[3])>0 for x in current.splitlines() if x.startswith('F ')),current
                results.append({'publisher_recreated_motor_no_reconnect':True})
                group.terminate();assert group.wait(timeout=8)==0
                assert k2.poll() is None,'Supervisor shutdown terminated standalone keyboard'
                os.write(master,b'\x1b');assert k2.wait(timeout=3)==0
                results.append({'supervisor_independence':True,'escape_final_zero':True})
            else:
                press('w');wait(lambda:any(samples[-1]['v']))
                sig={'sigint':signal.SIGINT,'sigterm':signal.SIGTERM,'sigkill':signal.SIGKILL}[scenario]
                killed=time.monotonic_ns();k.send_signal(sig);code=k.wait(timeout=3)
                zero=wait(lambda:events(killed,True));time.sleep(.03)
                if scenario=='sigkill':
                    assert code==-signal.SIGKILL
                    last=max(x['generated_ns'] for x in samples if any(x['v']))
                    delay=(zero[0]['received_ns']-last)/1e6;assert 250<=delay<600,delay
                    results.append({'sigkill_motor_timeout_ms':delay})
                else:
                    assert code==0 and samples[-1]['v']==[0,0,0]
                    assert termios.tcgetattr(slave)==saved
                    results.append({scenario+'_final_zero':True,'stop_ms':(zero[0]['received_ns']-killed)/1e6})
                assert group.poll() is None
                group.terminate();assert group.wait(timeout=8)==0
            assert not u.errors,u.errors
            assert all(b['sequence']>a['sequence'] and b['generated_ns']>a['generated_ns'] for a,b in zip(samples,samples[1:]))
            (out/(scenario+'_commands.json')).write_text(json.dumps(samples,indent=2))
            print('PASS',scenario,flush=True)
        finally:
            for proc in reversed(procs):
                if proc.poll() is None:
                    proc.terminate()
                    try:proc.wait(timeout=8)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait()
            u.close()
            for f in handles:f.close()
            for fd in fds:os.close(fd)
            libc=ctypes.CDLL(None)
            for suffix in ['/cmd_vel','/wheel_state','/wheel_odometry','/imu/raw_data','/imu/mag_raw','/ekf/odometry']:
                libc.shm_unlink(('/'+(prefix+suffix)[1:].replace('/','%2F')).encode())
    # Managed-mode rejection is checked using the actual Bringup.
    with open(out/'managed_rejection.log','w') as f:
        managed=subprocess.Popen([str(Path(a.build_dir)/'bringup/kcf_bringup'),str(executable)],stdout=f,stderr=subprocess.STDOUT)
        try:
            wait(lambda:'requires standalone' in (out/'managed_rejection.log').read_text())
            wait(lambda:'INITIALIZING -> ERROR' in (out/'managed_rejection.log').read_text())
            managed.terminate();assert managed.wait(timeout=8)==1
        finally:
            if managed.poll() is None:managed.terminate();managed.wait(timeout=8)
    results.append({'supervisor_registration_rejected':True})
    (out/'result.json').write_text(json.dumps(results,indent=2))
if __name__=='__main__':main()
