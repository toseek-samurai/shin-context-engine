"""Human progress on stderr, including live elapsed time on a real terminal."""
import errno
import json
import os
import pty
import subprocess
import sys

tool = sys.argv[1]
for language, expected in [('en_US.UTF-8', 'Projects checked: 2 / 3'),
                           ('ru_RU.UTF-8', 'Проверено проектов: 2 / 3')]:
    env = {**os.environ, 'LC_ALL': language, 'TERM': 'xterm-256color'}
    result = subprocess.run([tool, 'force'], env=env, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0 and json.loads(result.stdout) == {'ok': True}, result
    assert expected in result.stderr and '50%' in result.stderr and '\x1b' not in result.stderr, result.stderr
    silent = subprocess.run([tool, 'auto'], env=env, capture_output=True, text=True, timeout=10)
    assert silent.returncode == 0 and not silent.stderr and json.loads(silent.stdout) == {'ok': True}
    master, slave = pty.openpty()
    try:
        child = subprocess.Popen([tool, 'auto'], env=env, stdout=subprocess.PIPE, stderr=slave)
        os.close(slave)
        slave = -1
        chunks = []
        while True:
            try:
                part = os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not part:
                break
            chunks.append(part)
        output, _ = child.communicate(timeout=10)
        terminal = b''.join(chunks).decode()
        assert child.returncode == 0 and json.loads(output) == {'ok': True}
        assert expected in terminal and '\r\x1b[2K' in terminal and '· 1' in terminal, terminal
    finally:
        os.close(master)
        if slave >= 0:
            os.close(slave)
if sys.platform == 'darwin':
    # The real helper observes a child over stderr. Its disappearance cannot
    # turn a progress write into SIGPIPE and interrupt the installation journal.
    reader, writer = os.pipe()
    os.close(reader)
    try:
        abandoned = subprocess.run([tool, 'json'], stdout=subprocess.PIPE, stderr=writer, timeout=10)
        assert abandoned.returncode == 0 and json.loads(abandoned.stdout) == {'ok': True}, abandoned
    finally:
        os.close(writer)
print('EN/RU terminal progress, elapsed heartbeat and separate JSON receipt passed')
