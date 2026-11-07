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
    for mode in ('popup-noisy-ack', 'popup-split-ack', 'popup-refused-exit',
                 'noisy-ack', 'split-ack', 'receipt', 'stderr-error', 'refused-exit', 'wrong-pid', 'stderr-ack', 'oversized'):
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
            elif mode in ('stderr-error', 'refused-exit', 'popup-refused-exit'):
                assert reply['result']['error']['code'] == 'update-monitor-open', reply
                if mode == 'popup-refused-exit':
                    assert reply['popup_visible'], 'refused exit closed the Monitor popup'
            elif mode == 'oversized':
                assert 'exceeded its size limit' in reply['failure'], reply
            else:
                assert reply['failure'] or not reply['result'].get('ok', False), reply
    assert not (base / 'absent-home').exists(), 'fixture touched service state'
    for mode in ('project-ready', 'project-error', 'project-service-error'):
        project_cli = base / ('fake-' + mode)
        arguments = base / (mode + '.args')
        project_cli.write_text('#!/bin/sh\nif [ "$1" = client-targets ]; then printf \'%s\\n\' \'{"ok":true,"targets":[]}\'; exit 0; fi\n'
            + 'printf \'%s\\n\' "$@" > ' + shlex.quote(str(arguments)) + '\nexec ' + shlex.quote(str(tool))
            + ' --cli-helper ' + shlex.quote(mode) + ' fixture\n')
        project_cli.chmod(0o700)
        output = base / (mode + '.json')
        child = subprocess.run([str(tool), '--cli-host', str(project_cli), str(base / 'absent-home'), mode, str(output)],
            capture_output=True, text=True, timeout=15)
        assert child.returncode == 0, (mode, child.stderr)
        reply = json.loads(output.read_text())
        assert reply['progress_before_completion'] and reply['window_stayed_visible'] and reply['controls_locked'], reply
        assert reply['retry_enabled'] and reply['operation_released'], reply
        assert reply['failed'] == (mode != 'project-ready'), reply
        if reply['failed']: assert 'fixture-project-error' in reply['status'], reply
        assert '--progress-json' in arguments.read_text().splitlines()
        assert not (base / 'absent-home').exists(), 'project UI fixture touched service state'
print('Native Monitor pipes: open-popup exit, refused exit preserves popup, stderr noise, fragmented ack, receipt, error and PID/channel refusal passed')
