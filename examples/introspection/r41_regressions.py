import subprocess as sp
from pathlib import Path
import signal
import sys
root=Path(__file__).resolve().parents[2]
build=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'build/r41-clean'
logs=build/'r41-validation'
logs.mkdir(exist_ok=True)
cases=[
 ('r41',[str(build/'examples/introspection/kcf_shm_incarnation_test')]),
 ('r5',[str(build/'examples/introspection/kcf_service_introspection_test')]),
 ('r4',[str(build/'examples/introspection/kcf_dynamic_access_test')]),
 ('r3',[str(build/'examples/introspection/kcf_type_descriptor_test')]),
 ('r2b',[str(build/'examples/introspection/kcf_supervisor_discovery_test')]),
 ('named_cli',[str(build/'examples/introspection/kcf_supervisor_discovery_test'),'--cli',str(build/'bringup/kcf_bringup')]),
 ('r2a',[str(build/'examples/introspection/kcf_endpoint_registry_test')]),
 ('discovery',[str(build/'examples/introspection/kcf_runtime_discovery_test')]),
 ('lifecycle',[str(build/'examples/supervisor/kcf_runtime_lifecycle_test')]),
 ('runtime_health',['python3',str(root/'examples/supervisor/runtime_health_checks.py'),str(build)]),
 ('supervisor_loss',['python3',str(root/'examples/supervisor/supervisor_loss_checks.py'),str(build)]),
 ('reset',['python3',str(root/'examples/supervisor/reset_checks.py'),str(build)]),
 ('system_status',[str(build/'examples/supervisor/kcf_system_status_recovery_test')]),
 ('topic_recovery',[str(build/'examples/recovery/kcf_topic_recovery_test')]),
 ('parameter_recovery',[str(build/'examples/recovery/kcf_parameter_recovery_test')]),
 ('incomplete_topic',[str(build/'examples/recovery/kcf_incomplete_recovery_test'),'topic']),
 ('incomplete_parameter',[str(build/'examples/recovery/kcf_incomplete_recovery_test'),'parameter']),
]
failed=[]
for name,command in cases:
    with (logs/(name+'.log')).open('w') as log:
        try:
            result=sp.run(command,cwd=root,stdout=log,stderr=sp.STDOUT,timeout=200)
            code=result.returncode
        except sp.TimeoutExpired:
            code=124
    print(name, 'PASS' if code==0 else f'FAIL ({code})',flush=True)
    if code:
        print((logs/(name+'.log')).read_text()[-4000:],flush=True)
        failed.append(name)
        break
if not failed:
    import socket,time
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as candidate:
        candidate.bind(('127.0.0.1',0));port=candidate.getsockname()[1]
    with (logs/'service_server.log').open('w') as log:
        server=sp.Popen([str(build/'examples/service/kcf_service_server'),str(port)],stdout=log,stderr=sp.STDOUT,cwd=root)
        try:
            deadline=time.monotonic()+5
            while 'listening' not in (logs/'service_server.log').read_text():
                if server.poll() is not None or time.monotonic()>deadline: raise RuntimeError('Service startup failed')
                time.sleep(.02)
            with (logs/'service_client.log').open('w') as client_log:
                code=sp.run([str(build/'examples/service/kcf_service_client'),str(port),'1000'],stdout=client_log,stderr=sp.STDOUT,timeout=30,cwd=root).returncode
            if code:failed.append('legacy_service')
        finally:
            if server.poll() is None:server.terminate()
            try:code=server.wait(timeout=5)
            except sp.TimeoutExpired:server.kill();server.wait();code=124
            if code:failed.append('service_server')
    print('legacy_service', 'PASS' if not failed else 'FAIL',flush=True)
if not failed:
    with (logs/'integration_backend.log').open('w') as log:
        backend=sp.Popen([str(build/'examples/integration/kcf_integration_backend')],stdout=log,stderr=sp.STDOUT,cwd=root)
        try:
            with (logs/'integration_client.log').open('w') as client_log:
                result=sp.run([str(build/'examples/integration/kcf_integration_client')],stdout=client_log,stderr=sp.STDOUT,timeout=45,cwd=root)
            if result.returncode: failed.append('integration_client')
        finally:
            if backend.poll() is None: backend.send_signal(signal.SIGTERM)
            try: code=backend.wait(timeout=10)
            except sp.TimeoutExpired:
                backend.kill();backend.wait();code=124
            if code: failed.append('integration_backend')
    print('integration', 'PASS' if not failed else 'FAIL',flush=True)
print('FAILED:',failed,flush=True)
raise SystemExit(bool(failed))
