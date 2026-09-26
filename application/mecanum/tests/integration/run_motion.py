#!/usr/bin/env python3
"""Nonzero data-flow scenarios using the existing PTY harness and Tool probe."""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import threading
import time
from run_integration import Uarts

class MotionUarts(Uarts):
    def __init__(self):
        self.lock=threading.Lock();self.rpm=[0.0]*4;self.enc=[0.0]*4
        self.yaw=0.0;self.last=time.monotonic_ns();self.events=[];self.responses=[]
        super().__init__()
    def advance(self):
        now=time.monotonic_ns();dt=(now-self.last)/1e9;self.last=now
        for i in range(4):self.enc[i]+=self.rpm[i]*4320/60*dt
        # UART order Arduino 1,2,3,4 -> FL,RL,RR,FR; original sign -1.
        fl,rl,rr,fr=[-r*2*math.pi*0.041/60 for r in self.rpm]
        wz=(-fl+fr-rl+rr)/(4*0.1975)
        self.yaw+=wz*dt
        return now,wz
    def motor_reply(self,cmd,args):
        with self.lock:
            now,wz=self.advance()
            if cmd==b'm':
                values=list(map(float,args[1:]));assert len(values)==4 and all(math.isfinite(v) and abs(v)<100 for v in values)
                self.rpm=values;self.commands.append(values);self.events.append({'received_ns':now,'rpm_uart':values})
                return b'OK\r\n'
            if cmd==b'r':self.enc=[0.0]*4;return b'OK\r\n'
            if cmd==b'e':values=[round(x) for x in self.enc]
            elif cmd==b'z':values=list(self.rpm)
            else:raise AssertionError(args)
            self.responses.append({'time_ns':now,'command':cmd.decode(),'values_uart':values})
            return (' '.join(str(v) for v in values)+'\r\n').encode()
    def imu_frames(self):
        with self.lock:
            now,wz=self.advance();yaw=math.atan2(math.sin(self.yaw),math.cos(self.yaw))
        gyro=round(wz*180/math.pi/2000*32768)
        angle=max(-32768,min(32767,round(yaw/math.pi*32768)))
        return [(0x51,(0,0,2048)),(0x52,(0,0,gyro)),(0x53,(0,0,angle)),(0x54,(10,20,30))]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['build-dir','probe','producer','output-dir']:p.add_argument('--'+name,required=True)
    a=p.parse_args();out=Path(a.output_dir);out.mkdir(parents=True,exist_ok=True)
    prefix='/mecanum_motion_'+str(os.getpid());u=MotionUarts();group=None;producer=None;records=[]
    log=open(out/'bringup.log','w');producer_log=open(out/'producer.log','w')
    def snapshot():
        r=subprocess.run([a.probe,prefix],capture_output=True,text=True,timeout=5);assert r.returncode==0,r.stderr
        fields={}
        for line in r.stdout.splitlines():
            v=shlex.split(line)
            if v[0]=='F':fields.setdefault(v[1],{})[v[2]]=v[3]
            elif v[0]=='V':fields.setdefault(v[1],{})[v[2]]=v[3:]
        return {'observed_ns':time.monotonic_ns(),'wheel':fields.get(prefix+'/wheel_odometry',{}),
                'ekf':fields.get(prefix+'/ekf/odometry',{}),'imu':fields.get(prefix+'/imu/raw_data',{})}
    def wait(check,seconds=8):
        end=time.monotonic()+seconds;last={}
        while time.monotonic()<end:
            assert not u.errors,u.errors
            last=snapshot()
            if check(last):return last
            time.sleep(0.03)
        raise AssertionError(last)
    def active(s):return s['ekf'].get('valid')=='true' and s['wheel'].get('valid')=='true'
    def line():
        assert select.select([producer.stdout],[],[],6)[0],'producer deadline'
        return producer.stdout.readline().split()
    try:
        producer=subprocess.Popen([a.producer,prefix],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=producer_log,text=True,bufsize=1)
        assert line()==['READY']
        launcher=Path(__file__).resolve().parents[2]/'bringup/run.py'
        group=subprocess.Popen([sys.executable,str(launcher),'--build-dir',a.build_dir,'--motor-serial',u.paths[0],'--imu-serial',u.paths[1],
                                '--topic-prefix',prefix,'--application-name','mecanum_motion_'+str(os.getpid())],stdout=log,stderr=subprocess.STDOUT)
        wait(active)
        for name,cmd,axis,pose in [('forward',(0.12,0,0),'vx_m_s','x_m'),('lateral',(0,0.10,0),'vy_m_s','y_m'),('rotation',(0,0,0.40),'wz_rad_s','yaw_rad')]:
            baseline=wait(active);start=time.monotonic_ns()
            producer.stdin.write(' '.join(map(str,cmd))+' 3000\n');producer.stdin.flush()
            samples=[];end=time.monotonic()+2.7
            while time.monotonic()<end:
                sample=snapshot();samples.append(sample);assert not u.errors,u.errors
                time.sleep(0.07)
            done=line();assert done[0]=='DONE' and done[4]=='0',done
            first,last,count=map(int,done[1:4]);assert count>100 and last>first
            end=time.monotonic()+2
            while time.monotonic()<end:
                with u.lock:stops=[e for e in u.events if e['received_ns']>=last and all(x==0 for x in e['rpm_uart'])]
                if stops:break
                time.sleep(0.005)
            assert stops,'missing expiry zero'
            delay=(stops[0]['received_ns']-last)/1e6
            assert 250<=delay<600,delay # measured software budget, not a real-time guarantee
            with u.lock:
                events=[e for e in u.events if start<=e['received_ns']<=stops[0]['received_ns']]
                replies=[e for e in u.responses if start<=e['time_ns']<=stops[0]['received_ns']]
            moving=[e for e in events if any(x!=0 for x in e['rpm_uart'])];assert moving
            assert not any(not any(e['rpm_uart']) and moving[0]['received_ns']<e['received_ns']<last for e in events),'unexpected stop during fresh commands'
            rpm=moving[0]['rpm_uart'];expected_signs={'forward':[-1,-1,-1,-1],'lateral':[1,-1,1,-1],'rotation':[1,1,-1,-1]}[name]
            assert all(x*sign>0 for x,sign in zip(rpm,expected_signs)),rpm
            warm=[s for s in samples if s['observed_ns']>first+1000000000 and active(s)];assert warm
            observed=warm[-1]
            if name=='rotation':
                assert float(observed['imu']['angular_velocity_rad_s'][2])>0.2 and float(observed['imu']['rpy_rad'][2])>0.1,observed
            else:
                assert abs(float(observed['imu']['angular_velocity_rad_s'][2]))<0.01,observed
            for src in ['wheel','ekf']:
                assert float(observed[src][axis])>0.5*max(cmd),observed
                assert float(observed[src][pose])-float(baseline[src][pose])>0.1,observed
            assert any(r['command']=='z' and any(v!=0 for v in r['values_uart']) for r in replies)
            counts=[r['values_uart'] for r in replies if r['command']=='e'];assert len(counts)>2 and counts[0]!=counts[-1]
            record={'scenario':name,'cmd':cmd,'first_generated_ns':first,'last_generated_ns':last,'published':count,
                    'expiry_stop_ms':delay,'baseline':baseline,'observed':observed,'samples':samples,'uart_commands':events,'uart_responses':replies}
            records.append(record);(out/(name+'.json')).write_text(json.dumps(record,indent=2))
            print('PASS',name,'RPM',rpm,'expiry_ms',round(delay,3),flush=True)
            wait(lambda s:active(s) and abs(float(s['wheel']['vx_m_s']))<0.01 and abs(float(s['wheel']['vy_m_s']))<0.01 and abs(float(s['wheel']['wz_rad_s']))<0.02)
            time.sleep(0.3)
        group.send_signal(signal.SIGTERM);assert group.wait(timeout=8)==0
        producer.stdin.close();assert producer.wait(timeout=3)==0
        assert not u.errors,u.errors
        (out/'summary.json').write_text(json.dumps({'prefix':prefix,'passed':[r['scenario'] for r in records],'expiry_stop_ms':[r['expiry_stop_ms'] for r in records]},indent=2))
    finally:
        for proc in [group,producer]:
            if proc and proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=8)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
        u.close();log.close();producer_log.close()
        libc=ctypes.CDLL(None)
        for suffix in ['/cmd_vel','/wheel_state','/wheel_odometry','/imu/raw_data','/imu/mag_raw','/ekf/odometry']:
            libc.shm_unlink(('/'+(prefix+suffix)[1:].replace('%','%25').replace('/','%2F')).encode())
if __name__=='__main__':main()
