#!/usr/bin/env python3
"""Exercise the terminal adapter and a disposable service through a pseudo-terminal."""
import fcntl
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

tool, profiles = map(lambda p: str(Path(p).resolve()), sys.argv[1:])


class Terminal:
    def __init__(self, home, *options):
        self.master, self.slave = pty.openpty()
        self.before = termios.tcgetattr(self.slave)
        self.resize(28, 100)
        self.child = subprocess.Popen([tool, 'stats', '--home', str(home), *options],
                                      stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                      env={**os.environ, 'TERM': 'xterm-256color',
                                           'LC_ALL': 'en_US.UTF-8' if sys.platform == 'darwin' else 'C.UTF-8'},
                                      start_new_session=True)
        self.data = b''

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        if hasattr(self, 'child'):
            self.child.send_signal(signal.SIGWINCH)

    def pump(self, seconds=0.2):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([self.master], [], [], min(0.05, max(0, end-time.monotonic())))[0]:
                try:
                    self.data += os.read(self.master, 65536)
                except OSError:
                    break

    def expect(self, text, timeout=10):
        end = time.monotonic() + timeout
        while text.encode() not in self.data and time.monotonic() < end:
            self.pump()
            if self.child.poll() is not None:
                break
        assert text.encode() in self.data, (text, self.data[-2000:])

    def key(self, value):
        self.data = b''
        # Request a full repaint after the action. Otherwise curses can emit
        # only a changed suffix, which is not a complete visible screen.
        os.write(self.master, value + b'\x0c')
        self.pump()

    def close(self, sig=None):
        try:
            if self.child.poll() is None:
                if sig:
                    self.child.send_signal(sig)
                else:
                    os.write(self.master, b'q')
                deadline = time.monotonic() + 4
                # A terminal emulator drains output while the app restores the
                # screen. Waiting without reading can fill the small PTY buffer.
                while self.child.poll() is None and time.monotonic() < deadline:
                    self.pump(0.05)
                assert self.child.poll() is not None, 'TUI did not exit within four seconds'
            expected = 128 + sig if sig else 0
            assert self.child.returncode == expected, (self.child.returncode, self.data[-1000:])
            after = termios.tcgetattr(self.slave)
            # Darwin may mark pending input for reprocessing when canonical mode
            # returns. PENDIN is transient kernel state, not a changed setting.
            after[3] &= ~getattr(termios, 'PENDIN', 0)
            before = self.before.copy()
            before[3] &= ~getattr(termios, 'PENDIN', 0)
            assert after == before, 'terminal settings were not restored'
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait(timeout=3)
            os.close(self.master)
            os.close(self.slave)


def ce(home, *args):
    result = subprocess.run([tool, *args, '--home', str(home)], capture_output=True, text=True, timeout=40)
    assert result.returncode == 0, (args, result.stderr)
    return json.loads(result.stdout)


with tempfile.TemporaryDirectory(prefix='shin-tui-', dir='/tmp') as temporary:
    base = Path(temporary).resolve()
    home, root = base / 'state', base / 'Example Project Пример'
    root.mkdir()
    source = root / 'limits.cpp'
    source.write_text('int requestLimit() { return 17; }\n')
    missing = base / 'never-created'
    redirected = subprocess.run([tool, 'stats', '--home', str(missing)], capture_output=True, text=True, timeout=3)
    assert redirected.returncode == 2 and 'tui-terminal-required' in redirected.stderr
    assert not missing.exists()
    offline = Terminal(missing)
    try:
        offline.expect('Service offline')
        offline.key(b'5')
        offline.expect('Model usage')
        offline.expect('--usage-file')
        offline.key(b'4')
        offline.expect('Storage')
        offline.resize(8, 40)
        offline.expect('resize to at least')
        offline.resize(28, 100)
        offline.key(b'?')
        offline.expect('Keyboard')
    finally:
        offline.close(signal.SIGINT)
    assert not missing.exists(), 'viewing an absent service created state'
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT):
        process = Terminal(missing)
        process.expect('SHIN')
        process.close(sig)
    stalled_home = base / 'stalled'
    service_dir = stalled_home / 'service'
    stalled_home.mkdir(mode=0o700)
    service_dir.mkdir(mode=0o700)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        endpoint = service_dir / 'rpc.sock'
        listener.bind(str(endpoint))
        endpoint.chmod(0o600)
        listener.listen(1)
        listener.settimeout(5)
        stalled = Terminal(stalled_home)
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                request = connection.recv(16384)
                assert b'service-status' in request
                # Keep the authenticated local socket open without a response.
                # Quit must cancel the in-flight read, not wait its RPC timeout.
                stalled.expect('SHIN')
                stalled.close()
        except BaseException:
            if stalled.child.poll() is None:
                stalled.close(signal.SIGTERM)
            raise
    ce(home, 'init', '--root', str(root), '--profile-dir', profiles)
    try:
        ce(home, 'query', 'requestLimit', '--root', str(root))
        before = ce(home, 'service-status')
        registry = (home / 'registry/state.json').read_bytes()
        source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        usage_home = base / 'usage'
        usage_home.mkdir(mode=0o700)
        usage_file = usage_home / 'state.json'
        usage_file.write_text(json.dumps({'schema_version': 'shin.model-usage.v2', 'config_sha256': 'a'*64,
            'updated_at_unix_ms': int(time.time()*1000), 'projects': [{'id': 'demo', 'name': 'Demo usage'}], 'sources': {},
            'buckets': [{'date': datetime.now(timezone.utc).date().isoformat(), 'project': 'demo', 'client': 'codex',
                         'model': 'fixture-model', 'measurement': 'usage-event', 'input_tokens': 100, 'output_tokens': 20,
                         'cache_read_tokens': 60, 'cache_write_tokens': 0, 'reasoning_tokens': 5, 'records': 1}]}))
        usage_file.chmod(0o600)
        terminal = Terminal(home, '--usage-file', str(usage_file))
        try:
            terminal.expect('LIVE')
            terminal.key(b'2')
            terminal.expect('Example Project')
            terminal.expect('Пример')
            terminal.key(b'\n')
            terminal.expect(str(root))
            terminal.key(b'3')
            terminal.expect('Request timing')
            terminal.key(b't')
            terminal.expect('Recent requests')
            terminal.key(b'4')
            terminal.expect('Logical files:')
            terminal.key(b'5')
            terminal.expect('Model usage')
            terminal.expect('fixture-model')
            terminal.key(b'p')
            terminal.expect('Demo usage')
            terminal.key(b'1')
            terminal.expect('Service overview')
        finally:
            terminal.close()
        after = ce(home, 'service-status')
        assert before['instance_id'] == after['instance_id']
        assert before['telemetry']['requests_started'] == after['telemetry']['requests_started'], 'TUI triggered retrieval'
        assert registry == (home / 'registry/state.json').read_bytes()
        assert source_hash == hashlib.sha256(source.read_bytes()).hexdigest()
        reconnect = Terminal(home)
        try:
            reconnect.expect('LIVE')
            ce(home, 'service-stop')
            reconnect.data = b''
            reconnect.expect('Service offline')
            ce(home, 'service-start')
            reconnect.data = b''
            reconnect.expect('LIVE')
        finally:
            reconnect.close()
    finally:
        ce(home, 'service-stop')
print(json.dumps({'ok': True, 'pty': True, 'tabs': 5, 'signals': ['INT', 'TERM', 'HUP', 'QUIT'],
                  'resize': True, 'unicode': True, 'pending_rpc_cancelled': True,
                  'terminal_restored': True, 'service_and_source_preserved': True}))

with tempfile.TemporaryDirectory(prefix='shin-stats-language-') as directory:
    screen = Terminal(Path(directory) / 'missing-home', '--language', 'ru')
    try:
        screen.expect('НЕТ СВЯЗИ')
        screen.key(b'?'); screen.expect('Клавиши')
        screen.key(b'l'); screen.expect('Keyboard')
        screen.key(b'l'); screen.expect('Клавиши')
        screen.resize(14, 60); screen.key(b'\x0c'); screen.expect('Клавиши')
    finally:
        screen.close()
    bad = subprocess.run([tool, 'stats', '--language', 'fr'], capture_output=True, text=True)
    assert bad.returncode == 2 and json.loads(bad.stderr)['error']['code'] == 'invalid-arguments'
