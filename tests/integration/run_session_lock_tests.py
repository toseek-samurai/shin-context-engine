"""Verify receipt TTL includes time spent waiting for a snapshot publication lock."""
import fcntl
import json
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    tool, project, profiles, output, draft_path = sys.argv[1:]
    route = [tool, '--profile', 'fixture', '--profile-dir', profiles,
             '--root', project, '--out-dir', output]
    binding = ['--task-id', 'lock-test', '--thread-id', 'lock-thread']
    draft = json.loads(pathlib.Path(draft_path).read_text())
    draft['state']['assertions'] = []
    draft['receipts'] = []
    capture = route + ['checkpoint', '--input', '-'] + binding
    first = json.loads(subprocess.run(capture, input=json.dumps(draft), text=True,
                                     capture_output=True, check=True).stdout)
    now = int(time.time())
    scope = dict(first['binding'], snapshot_id=first['baseline']['snapshot_id'],
                 worktree_id=first['baseline']['worktree_id'])
    draft['receipts'] = [{'schema_version': 'shin.execution-receipt.v1', 'receipt_id': 'short-ttl',
                          'kind': 'test', 'producer': 'lock-runner', 'observed_at': now,
                          'ttl_seconds': 2, 'result': 'pass', 'scope': scope,
                          'command_fingerprint': 'fnv1a64:0000000000000000'}]
    checkpoint = subprocess.run(capture, input=json.dumps(draft), text=True,
                                capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix='shin-lock-session-') as temporary:
        checkpoint_path = pathlib.Path(temporary) / 'checkpoint.json'
        checkpoint_path.write_text(checkpoint)
        lock_path = pathlib.Path(output) / 'docs' / 'kb' / '.shin-context.lock'
        with lock_path.open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            command = route + ['resume', '--checkpoint', str(checkpoint_path)] + binding
            command += ['--trusted-producer', 'lock-runner', '--require-current']
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                # TTL uses wall time; a fixed sleep is not a wall-clock barrier on a VM.
                deadline = time.monotonic() + 30
                while time.time() < now + 4:
                    assert process.poll() is None, 'resume did not wait for the publication lock'
                    assert time.monotonic() < deadline, 'host wall clock did not advance before the test deadline'
                    time.sleep(0.05)
                assert process.poll() is None, 'resume did not wait for the publication lock'
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
            stdout, stderr = process.communicate(timeout=30)
        assert process.returncode == 0, stderr
        result = json.loads(stdout)
        receipt = result['receipts'][0]
        assert result['observed_at'] >= now + 2, {'receipt_time': now, 'assessment_time': result['observed_at']}
        assert receipt['age'] == 'expired' and not receipt['passed_current'], receipt
        assert receipt['verification_level'] == 'producer-asserted', receipt
        assert result['review_required'], result


if __name__ == '__main__':
    main()
