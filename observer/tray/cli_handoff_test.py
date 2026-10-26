"""Real Monitor CLI pipes and AppKit exit, without an installation or network."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

tool = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='shin-monitor-cli-') as temporary:
    base = Path(temporary)
    cli = base / 'fake shin-ce'
    cli.write_text('#!/bin/sh\nexec ' + shlex.quote(str(tool)) + ' --cli-helper "$@"\n')
    cli.chmod(0o700)
    for mode in ('noisy-ack', 'split-ack', 'receipt', 'stderr-error', 'refused-exit', 'wrong-pid', 'stderr-ack', 'oversized'):
        output = base / (mode + '.json')
        child = subprocess.run([str(tool), '--cli-host', str(cli), str(base / 'absent-home'), mode, str(output)],
                               capture_output=True, text=True, timeout=15)
        assert child.returncode == 0, (mode, child.returncode, child.stderr)
        if mode.endswith('ack') and mode != 'stderr-ack':
            receipt = Path(str(output) + '.handoff')
            deadline = time.monotonic() + 8
            while not receipt.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            assert receipt.exists() and json.loads(receipt.read_text())['parent_exited'], (mode, child.stderr)
            assert not output.exists(), 'accepted handoff called the failure completion'
        else:
            reply = json.loads(output.read_text())
            if mode == 'receipt':
                assert reply['result'].get('value') == 17 and not reply['failure'], reply
            elif mode in ('stderr-error', 'refused-exit'):
                assert reply['result']['error']['code'] == 'update-monitor-open', reply
            elif mode == 'oversized':
                assert 'exceeded its size limit' in reply['failure'], reply
            else:
                assert reply['failure'] or not reply['result'].get('ok', False), reply
    assert not (base / 'absent-home').exists(), 'fixture touched service state'
print('Native Monitor pipes: stderr noise, fragmented ack, actual exit, refused exit, receipt, error and PID/channel refusal passed')
