#!/usr/bin/env python3
"""KCF's existing CLI only; no process lifecycle or restart API is reimplemented."""
import argparse
import os
from pathlib import Path

def command(args):
    build = Path(args.build_dir).resolve()
    specs = {
        'motor': [str(build/'application/mecanum/kcf_mecanum_motor'), '--serial', args.motor_serial,
                  '--baud', str(args.motor_baud), '--topic-prefix', args.topic_prefix],
        'imu': [str(build/'application/mecanum/imu/kcf_mecanum_imu'), '--serial', args.imu_serial,
                '--baud', str(args.imu_baud), '--topic-prefix', args.topic_prefix],
        'ekf': [str(build/'application/mecanum/ekf/kcf_mecanum_ekf'),
                '--wheel-topic', args.topic_prefix+'/wheel_odometry',
                '--imu-topic', args.topic_prefix+'/imu/raw_data',
                '--output-topic', args.topic_prefix+'/ekf/odometry',
                '--imu-yaw-relative', args.imu_yaw_relative],
    }
    result = [str(build/'bringup/kcf_bringup'), '--application-name', args.application_name]
    components = specs if args.component == 'all' else [args.component]
    for i, name in enumerate(components):
        if i: result.append('--next')
        result += specs[name] + ['--startup-timeout-ms', '7000', '--health-timeout-ms', '2000', '--shutdown-timeout-ms', '2000']
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', required=True)
    p.add_argument('--motor-serial', required=True)
    p.add_argument('--imu-serial', required=True)
    p.add_argument('--motor-baud', type=int, default=115200)
    p.add_argument('--imu-baud', type=int, default=115200)
    p.add_argument('--topic-prefix', default='/mecanum')
    p.add_argument('--application-name', default='mecanum')
    p.add_argument('--component', choices=['all','motor','imu','ekf'], default='all')
    p.add_argument('--imu-yaw-relative', choices=['true','false'], default='true')
    args=p.parse_args()
    if not args.topic_prefix.startswith('/') or args.topic_prefix.endswith('/') or args.motor_baud<=0 or args.imu_baud<=0:
        p.error('invalid Topic prefix or baudrate')
    cmd=command(args)
    os.execv(cmd[0],cmd)
if __name__=='__main__': main()
