#!/usr/bin/env python3
"""Run after build. Test executable calls RequestReset; no production signal trigger."""
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1] if len(sys.argv) > 1 else 'build').resolve()
root = build / 'examples/supervisor'
for mode in ('repeat', 'failure', 'execfailure', 'shutdown', 'shutdown-new', 'incomplete'):
    with tempfile.TemporaryFile(mode='w+') as log:
        result = subprocess.run([str(root/'kcf_reset_test'), str(root/'kcf_supervisor_safe'), mode],
                                stdout=log, stderr=subprocess.STDOUT, timeout=100)
        log.seek(0)
        text = log.read()
        assert result.returncode == 0, text
        assert 'ERROR -> RUNNING' not in text, text
        if mode == 'repeat':
            assert text.count('Reset complete') == 10, text
            assert text.count('force stop name=B') >= 2, text
            assert 'signal=9' in text, text
            # New SafeElement starts at zero throughout delayed initialization.
            for segment in text.split('ERROR -> RESETTING')[1:]:
                initializing = segment.split('RESETTING -> RUNNING')[0]
                assert 'Setup output=0' in initializing, text
                assert 'alive safe=0 output=0' in initializing, text
                assert 'alive safe=0 output=100' not in initializing, text
        if mode.startswith('shutdown'):
            shutdown = text.split('RESETTING -> SHUTTING_DOWN')[1]
            assert 'started name=' not in shutdown and 'Reset complete' not in shutdown, text
        print(f'Reset {mode} PASS', flush=True)
