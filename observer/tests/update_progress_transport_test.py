"""The private progress viewer reads bounded frames; it never opens a project."""
from pathlib import Path
import json
import subprocess
import sys
import time

tool = Path(sys.argv[1]).resolve()
for arguments in (['--update-progress'], ['--update-progress', '2.0.5', 'de'],
                  ['--update-progress', 'bad\nversion', 'en']):
    child = subprocess.run([str(tool), *arguments], stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
    assert child.returncode == 2, (arguments, child.returncode)

for language in ('en', 'ru'):
    child = subprocess.Popen([str(tool), '--update-progress', '2.0.5-preview', language],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # Fragmented records, malformed payloads and unknown stages cannot launch an installer.
        child.stdin.write(b'{"step":0,"received":0,')
        child.stdin.flush()
        child.stdin.write(b'"total":0}\nnot-json\n{"step":99,"received":0,"total":0}\n')
        child.stdin.write((json.dumps({'step': 2, 'received': 5, 'total': 10})+'\n').encode())
        child.stdin.flush()
        time.sleep(.3)
        assert child.poll() is None, 'viewer ended before its private channel closed'
        child.stdin.close(); child.stdin = None
        output, error = child.communicate(timeout=10)
        assert child.returncode == 0 and not output, (child.returncode, output, error)
    finally:
        if child.poll() is None:
            child.kill(); child.communicate(timeout=5)

child = subprocess.run([str(tool), '--update-progress', '2.0.5-preview', 'en'],
                       input=b'x'*8193, capture_output=True, timeout=10)
assert child.returncode == 0 and not child.stdout, 'unbounded or malformed channel was not closed'
print('Private progress viewer framing, language, EOF and malformed-input bounds passed')
