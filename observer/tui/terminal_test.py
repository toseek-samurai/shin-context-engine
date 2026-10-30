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

tool, profiles = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])


class Terminal:
    def __init__(self, home, *options, environment=None):
        self.master, self.slave = pty.openpty()
        self.before = termios.tcgetattr(self.slave)
        self.resize(28, 100)
        self.child = subprocess.Popen([tool, 'stats', '--home', str(home), *options],
                                      stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                      env={**os.environ, 'TERM': 'xterm-256color',
                                           'LC_ALL': 'en_US.UTF-8' if sys.platform == 'darwin' else 'C.UTF-8',
                                           **(environment or {})},
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

    def key(self, value, redraw=True):
        self.data = b''
        # Request a full repaint after the action. Otherwise the UI can emit
        # only a changed suffix, which is not a complete visible screen.
        os.write(self.master, value + (b'\x0c' if redraw else b''))
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
        offline.key(b'u')
        if sys.platform == 'darwin':
            offline.expect('Check for a newer complete release')
            offline.key(b'r')
            offline.expect('No owned installation exists at this prefix.')
        else:
            offline.expect('Automatic release updates are currently available on macOS only.')
        offline.key(b'3')
        offline.key(b']')
        offline.expect('Model usage')
        offline.expect('--usage-file')
        offline.key(b'4')
        offline.expect('General')
        offline.key(b']')
        offline.expect('Storage')
        offline.resize(8, 40)
        offline.expect('resize to at least')
        offline.resize(28, 100)
        offline.key(b'?')
        offline.expect('Keyboard')
    finally:
        offline.close(signal.SIGINT)
    assert not missing.exists(), 'viewing an absent service created state'
    for no_color in ('', '1'):
        colored = Terminal(missing, environment={'NO_COLOR': no_color, 'COLORTERM': 'truecolor'})
        try:
            colored.expect('SHIN')
            assert (b'38;2;228;204;162' in colored.data) == (not no_color), 'Sand color capability / NO_COLOR mismatch'
        finally:
            colored.close()
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
            # Left-click Projects in the fixed top navigation row (SGR mouse).
            terminal.key(b'\x1b[<0;38;4M')
            terminal.expect('Example Project')
            terminal.expect('Пример')
            terminal.key(b'\n')
            terminal.expect(str(root))
            terminal.key(b'3')
            terminal.expect('Request timing')
            terminal.key(b't')
            terminal.expect('Recent requests')
            terminal.key(b'4')
            terminal.expect('General')
            terminal.key(b'\x1b[<0;10;6M')
            terminal.expect('Общие')
            terminal.key(b'l')
            terminal.expect('General')
            terminal.key(b'\x1b[C')
            terminal.expect('Logical files:')
            terminal.key(b'3')
            terminal.expect('Request timing')
            terminal.key(b']')
            terminal.expect('Model usage')
            terminal.expect('fixture-model')
            terminal.key(b'p')
            terminal.expect('Demo usage')
            terminal.key(b']')
            terminal.expect('Sessions · retained MCP observations')
            terminal.key(b'4')
            terminal.expect('Logical files:')
            terminal.key(b'1')
            terminal.expect('Memory (service + workers)')
            terminal.key(b'l')
            terminal.expect('Память')
            terminal.key(b'l')
            terminal.expect('Memory (service + workers)')
            terminal.key(b'g')
            terminal.expect('Active queries')
            terminal.key(b'h')
            terminal.expect('Requests per interval')
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
        # The RPC acknowledges a stop request, not process termination. Wait for
        # the owned endpoint to close before deleting its history directory.
        deadline = time.monotonic() + 10
        while (home / 'service/rpc.sock').exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not (home / 'service/rpc.sock').exists(), 'service did not stop before fixture cleanup'
print(json.dumps({'ok': True, 'pty': True, 'sections': 4, 'pages': 8, 'signals': ['INT', 'TERM', 'HUP', 'QUIT'],
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

# Same real event loop with an injected, non-installing native operation. The
# fixture executable is test-only; production has no simulation flag or env hook.
if len(sys.argv) > 3:
    real_tool, tool = tool, str(Path(sys.argv[3]).resolve())
    try:
        with tempfile.TemporaryDirectory(prefix='shin-tui-update-flow-') as directory:
            home = Path(directory) / 'absent-home'
            screen = Terminal(home)
            try:
                screen.expect('SHIN')
                screen.key(b'u'); screen.expect('Check for a newer complete release')
                screen.key(b'r'); screen.expect('2.0.5-dev.99')
                screen.key(b'\n'); screen.expect('A Shin update is available')
                screen.key(b'i'); screen.expect('Install the reviewed release?')
                screen.key(b'3'); screen.expect('Request timing')
                screen.key(b'4'); screen.expect('[i Install]')
                assert b'Install the reviewed release?' not in screen.data, 'section change retained confirmation'
                screen.key(b'y'); screen.expect('A Shin update is available')
                screen.key(b'i'); screen.expect('Install the reviewed release?')
                # Exercise a delayed input reader. A fixed pump interval cannot
                # prove that FTXUI consumed standalone ESC: queuing Ctrl-L next
                # would turn it into Alt-Ctrl-L. Wait for the cancelled offer's
                # action before sending any more input.
                screen.child.send_signal(signal.SIGSTOP)
                _, stopped = os.waitpid(screen.child.pid, os.WUNTRACED)
                assert os.WIFSTOPPED(stopped), 'TUI fixture did not stop'
                try:
                    screen.key(b'\x1b', redraw=False)
                finally:
                    screen.child.send_signal(signal.SIGCONT)
                screen.expect('[i Install]')
                screen.key(b'\x0c'); screen.expect('A Shin update is available')
                assert b'Install the reviewed release?' not in screen.data, 'Esc did not cancel confirmation'
                screen.key(b'y'); screen.expect('A Shin update is available')
                screen.key(b'i'); screen.expect('Install the reviewed release?')
                screen.key(b'y'); screen.expect('Downloading the update')
                screen.expect('50%')
                screen.key(b'q')
                assert screen.child.poll() is None, 'q interrupted a native update'
                screen.key(b'\x03')
                assert screen.child.poll() is None, 'Ctrl-C interrupted a native update'
                screen.expect('Projects checked: 2 / 3')
                screen.expect('Shin was updated')
                screen.key(b'v'); screen.expect('Update needs review')
                screen.key(b'a'); screen.expect('No pending update result')
            finally:
                screen.close()
            assert b'fixture-installs=1 acknowledgements=1' in screen.data, screen.data[-3000:]
            assert not home.exists(), 'TUI fixture mutated service data'
    finally:
        tool = real_tool
    print('TUI update flow: explicit confirmation, cancellation, actual counters, quit guard, receipt, acknowledgement and terminal restore passed')
