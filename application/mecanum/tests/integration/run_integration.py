#!/usr/bin/env python3
"""Real process / PTY integration. Never opens a physical UART."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import pty
import select
import shlex
import signal
import struct
import subprocess
import sys
import threading
import time
import tty

class Uarts:
    def __init__(self):
        self.motor,self.motor_slave=pty.openpty()
        self.imu,self.imu_slave=pty.openpty()
        tty.setraw(self.motor_slave); tty.setraw(self.imu_slave)
        self.paths=[os.ttyname(self.motor_slave),os.ttyname(self.imu_slave)]
        self.running=True; self.imu_enabled=True; self.count=0; self.commands=[]; self.errors=[]
        self.threads=[threading.Thread(target=self.motor_loop),threading.Thread(target=self.imu_loop)]
        for t in self.threads:t.start()
    def motor_loop(self):
        buf=b''
        try:
            while self.running:
                if not select.select([self.motor],[],[],0.02)[0]:continue
                buf+=os.read(self.motor,4096)
                while b'\r' in buf:
                    line,buf=buf.split(b'\r',1); args=line.strip().split()
                    if not args:continue
                    cmd=args[0]
                    reply=self.motor_reply(cmd,args)
                    os.write(self.motor,reply)
        except Exception as e:self.errors.append(repr(e))
    def motor_reply(self,cmd,args):
        if cmd==b'm':
            values=list(map(float,args[1:])); assert len(values)==4 and all(v==0 for v in values),values
            self.commands.append(values);return b'OK\r\n'
        if cmd==b'r':self.count=0;return b'OK\r\n'
        if cmd==b'e':
            self.count-=1
            return (' '.join([str(self.count)]*4)+'\r\n').encode()
        if cmd==b'z':return b'0 0 0 0\r\n'
        raise AssertionError(('unexpected UART command',args))
    def imu_frames(self):
        return [(0x51,(0,0,2048)),(0x52,(0,0,0)),(0x53,(0,0,12743)),(0x54,(10,20,30))]
    def imu_loop(self):
        try:
            while self.running:
                if self.imu_enabled:
                    for kind,values in self.imu_frames():
                        frame=bytes([0x55,kind])+struct.pack('<hhhh',*values,0)
                        os.write(self.imu,frame+bytes([sum(frame)&255]))
                time.sleep(0.02)
        except Exception as e:self.errors.append(repr(e))
    def close(self):
        self.running=False
        for t in self.threads:t.join(2)
        for fd in [self.motor,self.motor_slave,self.imu,self.imu_slave]:os.close(fd)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',required=True);p.add_argument('--probe',required=True);p.add_argument('--output-dir',required=True)
    a=p.parse_args();out=Path(a.output_dir);out.mkdir(parents=True,exist_ok=True)
    launcher=Path(__file__).resolve().parents[2]/'bringup/run.py'
    prefix='/mecanum_integration_'+str(os.getpid());u=Uarts(); processes=[];results=[];counter=0
    def start(component='all',bad=False):
        nonlocal counter
        counter+=1;name='mecanum_integration_'+str(os.getpid())+'_'+str(counter)
        log=out/(name+'.log');f=open(log,'w')
        cmd=[sys.executable,str(launcher),'--build-dir',a.build_dir,'--motor-serial',('/tmp/nonexistent_mecanum_uart_'+str(os.getpid()) if bad else u.paths[0]),
             '--imu-serial',u.paths[1],'--topic-prefix',prefix,'--component',component,'--application-name',name]
        proc=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT);processes.append((proc,f,log));return proc,name,log
    def snapshot():
        r=subprocess.run([a.probe,prefix],capture_output=True,text=True,timeout=5)
        assert r.returncode==0,r.stderr
        data={'apps':{},'runtimes':{},'topics':{},'fields':{}}
        for line in r.stdout.splitlines():
            x=shlex.split(line)
            if x[0]=='A':data['apps'][x[1]]={'state':x[2],'count':int(x[3])}
            elif x[0]=='R':data['runtimes'][x[1]]={'exe':x[2],'state':x[3],'mode':x[4]}
            elif x[0]=='T':data['topics'][x[1]]={'error':int(x[2]),'type':x[3],'sequence':int(x[4]),'fields':int(x[5]),'pid':int(x[6])}
            elif x[0]=='F':data['fields'].setdefault(x[1],{})[x[2]]=x[3]
        return data
    def wait(check,label,seconds=10):
        end=time.monotonic()+seconds;last={}
        while time.monotonic()<end:
            assert not u.errors,u.errors
            last=snapshot()
            if check(last):
                (out/(label+'.json')).write_text(json.dumps(last,indent=2));results.append(label);print('PASS',label,flush=True);return last
            time.sleep(0.1)
        raise AssertionError((label,last))
    def estimate(s):return s['fields'].get(prefix+'/ekf/odometry',{})
    def valid(s):return estimate(s).get('valid')=='true'
    def stop(proc,expected=0):
        proc.send_signal(signal.SIGTERM);rc=proc.wait(timeout=8);assert rc==expected,(proc.pid,rc,expected)
    def children(s):
        # The probe's Topic list is restricted to this test's unique prefix.
        owners={topic['pid'] for topic in s['topics'].values()}
        return {Path(v['exe']).name:int(pid) for pid,v in s['runtimes'].items() if int(pid) in owners and Path(v['exe']).name in ['kcf_mecanum_motor','kcf_mecanum_imu','kcf_mecanum_ekf'] and v['state']=='RUNNING'}
    try:
        group,name,log=start()
        first=wait(lambda s:valid(s) and s['apps'].get(name,{}).get('state')=='RUNNING','group_running')
        ids=children(first);assert len(ids)==3 and len(set(ids.values()))==3,ids
        for pid in ids:
            assert first['runtimes'][str(ids[pid])]['mode']=='SUPERVISED',first['runtimes']
        for suffix in ['/wheel_state','/wheel_odometry','/imu/raw_data','/imu/mag_raw','/ekf/odometry']:
            info=first['topics'][prefix+suffix];assert info['error']==0 and info['fields']>0 and info['type'],info
        for suffix,exe in [('/wheel_odometry','kcf_mecanum_motor'),('/imu/raw_data','kcf_mecanum_imu'),('/ekf/odometry','kcf_mecanum_ekf')]:
            assert first['topics'][prefix+suffix]['pid']==ids[exe]
        initial_seq=int(estimate(first)['sequence'])
        wait(lambda s:valid(s) and int(estimate(s).get('sequence','0'))>initial_seq+5,'independent_publishers_progress')
        u.imu_enabled=False
        wait(lambda s:estimate(s).get('valid')=='false','input_silence_invalid')
        u.imu_enabled=True
        wait(valid,'input_resume_valid')
        stop(group)
        text=log.read_text();assert text.count('reaped name=')==3 and 'force stop' not in text and 'Shutdown complete' in text,text
        results.append('group_normal_shutdown')
        # Isolated restart: each existing Bringup owns exactly one child.
        motor,mname,mlog=start('motor');imu,iname,ilog=start('imu');ekf,ename,elog=start('ekf')
        live=wait(lambda s:valid(s) and len(children(s))==3,'isolated_supervisors_running');session=estimate(live)['session_id'];oldids=children(live)
        stop(ekf);ekf,ename,elog=start('ekf')
        live=wait(lambda s:valid(s) and len(children(s))==3 and estimate(s).get('session_id')!=session,'ekf_only_restart')
        assert children(live)['kcf_mecanum_motor']==oldids['kcf_mecanum_motor'] and children(live)['kcf_mecanum_imu']==oldids['kcf_mecanum_imu']
        for which,proc in [('imu',imu),('motor',motor)]:
            generation=('wheel' if which=='motor' else 'imu')+'_generation'
            old=estimate(live)[generation];session=estimate(live)['session_id']
            stop(proc)
            wait(lambda s:estimate(s).get('valid')=='false',which+'_stopped_invalid')
            new,n,lg=start(which)
            if which=='imu':imu=new
            else:motor=new
            live=wait(lambda s:valid(s) and len(children(s))==3 and int(estimate(s).get(generation,'0'))>int(old),which+'_restart_reconnected')
            assert estimate(live)['session_id']==session
        # Abrupt termination observed by the existing supervisor and Tool.
        victim=children(live)['kcf_mecanum_imu'];os.kill(victim,signal.SIGKILL)
        failed=wait(lambda s:estimate(s).get('valid')=='false' and any(v['state']=='ERROR' for n,v in s['apps'].items() if n.startswith('mecanum_integration_'+str(os.getpid()))),'sigkill_error_and_invalid')
        stop(imu,1);stop(ekf);stop(motor)
        bad,bname,blog=start('motor',True);assert bad.wait(timeout=8)!=0
        assert 'UART initialization failed' in blog.read_text();results.append('missing_uart_initialization_failure')
        assert u.commands and all(all(v==0 for v in x) for x in u.commands)
        (out/'result.json').write_text(json.dumps({'passed':results,'zero_commands':len(u.commands),'prefix':prefix,'initial_pids':ids,'tool':'existing KcfBackend Refresh/GetElements/StartTopicEcho/ReadTopicEcho'},indent=2))
        print('PASS all integration checks',flush=True)
    finally:
        for proc,f,log in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=8)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
            f.close()
        u.close()
        # Remove only this test's named Topic objects, including SIGKILL leftovers.
        libc=ctypes.CDLL(None)
        for suffix in ['/wheel_state','/wheel_odometry','/imu/raw_data','/imu/mag_raw','/ekf/odometry']:
            logical=prefix+suffix
            name='/'+logical[1:].replace('%','%25').replace('/','%2F')
            libc.shm_unlink(name.encode())
if __name__=='__main__':main()
