"""Native product acceptance: no model, WSL daemon, fake server or live project."""
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

CE, MCP, PROFILES = (Path(value).resolve() for value in sys.argv[1:4])
SID = re.search(r'S-1-5-[0-9-]+', subprocess.check_output(
    ['whoami', '/user', '/fo', 'csv', '/nh'], text=True)).group()


def private(path, unsafe=False):
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    convert.restype = ctypes.c_int
    apply = advapi.SetFileSecurityW
    apply.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_void_p]
    apply.restype = ctypes.c_int
    descriptor = ctypes.c_void_p()
    sddl = f'O:{SID}D:P(A;;FA;;;{SID})' + ('(A;;FR;;;WD)' if unsafe else '')
    assert convert(sddl, 1, ctypes.byref(descriptor), None), ctypes.get_last_error()
    try:
        assert apply(str(path), 0x80000005, descriptor), ctypes.get_last_error()
    finally:
        free = ctypes.WinDLL('kernel32').LocalFree
        free.argtypes = [ctypes.c_void_p]
        free(descriptor)


def await_condition(probe, label, seconds=40):
    end = time.monotonic() + seconds
    last = None
    wake = threading.Event()
    while time.monotonic() < end:
        last = probe()
        if last:
            return last
        wake.wait(0.05)
    raise AssertionError(f'{label} timed out: {last}')


class Adapter:
    def __init__(self, executable, root, home, roots=None):
        argv = [str(executable), '--home', str(home)]
        for selected in roots or [root]:
            argv += ['--root', str(selected)]
        self.child = subprocess.Popen(argv,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.rows = queue.Queue()
        self.serial = 0
        self.pending = {}
        def read():
            for line in self.child.stdout:
                self.rows.put(json.loads(line))
            self.rows.put(None)
        self.reader = threading.Thread(target=read)
        self.reader.start()

    def call(self, method, params):
        return self.response(self.send(method, params))

    def send(self, method, params):
        self.serial += 1
        self.child.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': self.serial,
            'method': method, 'params': params}) + '\n').encode())
        self.child.stdin.flush()
        return self.serial

    def response(self, request_id):
        if request_id in self.pending:
            return self.pending.pop(request_id)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            row = self.rows.get(timeout=max(0.001, end - time.monotonic()))
            assert row is not None, 'MCP disconnected'
            if row.get('id') == request_id:
                return row
            if 'id' in row:
                self.pending[row['id']] = row
        raise AssertionError('MCP response timeout')

    def tool(self, name, args):
        result = self.call('tools/call', {'name': name, 'arguments': args})['result']
        value = json.loads(result['content'][0]['text'])
        assert result.get('isError', False) == (not value['ok'])
        return value

    def close(self):
        if self.child.stdin:
            self.child.stdin.close()
            self.child.stdin = None
        try:
            self.child.wait(timeout=8)
            assert self.child.returncode == 0, self.child.stderr.read().decode(errors='replace')
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait(timeout=5)
            self.reader.join(timeout=5)


def scenario(base):
    kit = base / 'native kit'
    (kit / 'bin').mkdir(parents=True)
    private(kit)
    private(kit / 'bin')
    registry = kit / 'share' / 'shin-context' / 'profiles'
    registry.mkdir(parents=True)
    shutil.copyfile(PROFILES / 'generic-source.json', registry / 'generic-source.json')
    for source, name in ((CE, 'shin-ce.exe'), (MCP, 'shin-mcp.exe')):
        target = kit / 'bin' / name
        shutil.copyfile(source, target)
        private(target)
    tool = kit / 'bin' / 'shin-ce.exe'
    adapter_tool = kit / 'bin' / 'shin-mcp.exe'
    home = base / 'home'
    first, second, foreign = (base / name for name in ('project A', '\u043f\u0440\u043e\u0435\u043a\u0442 B', 'foreign'))
    for root, value in ((first, 17), (second, 99), (foreign, 77)):
        root.mkdir()
        (root / 'README.md').write_text('# Quota\n\nquotaLimit owns the quota.\n', encoding='utf-8')
        (root / 'quota.cpp').write_text(f'int quotaLimit() {{ return {value}; }}\n', encoding='utf-8')

    def ce(*args, okay=True):
        result = subprocess.run([str(tool), *map(str, args), '--home', str(home)],
            capture_output=True, encoding='utf-8', timeout=75)
        raw = result.stdout if result.stdout.strip() else result.stderr
        try:
            value = json.loads(raw)
        except ValueError:
            raise AssertionError((result.returncode, result.stdout, result.stderr))
        if okay:
            assert result.returncode == 0 and value['ok'], value
        return value

    def status():
        return ce('service-status')

    def stop():
        answer = ce('service-stop', okay=False)
        if answer['ok']:
            assert answer['stop_requested']
        await_condition(lambda: not ce('service-status', okay=False)['ok'], 'service stop', 15)

    def query(root, expected):
        def probe():
            result = ce('query', 'quotaLimit', '--root', root, okay=False)
            if not result['ok']:
                assert result.get('error', {}).get('code') in {
                    'project-updating', 'project-busy', 'service-busy', 'source-read-hash-mismatch'}, result
                return None
            return result if expected in result.get('source_context', '') else None
        return await_condition(probe, f'query {root.name}')

    try:
        print('RUN product init/background service', flush=True)
        a = ce('init', '--root', first)
        assert a['local_ready'] and a['service_enrolled'] and a['daemon_started'], a
        instance = status()['instance_id']
        b = ce('init', '--root', second)
        assert b['local_ready'] and b['service_enrolled'] and not b['daemon_started'], b
        assert status()['instance_id'] == instance
        initial_identity = ce('identity', '--root', first)['identity']
        print('RUN case alias of an existing project', flush=True)
        alias = ce('init', '--root', str(first).upper())
        assert alias['root'] == a['root'], ('same physical directory acquired another binding', alias['root'], a['root'])
        assert ce('identity', '--root', str(first).upper())['identity'] == initial_identity
        assert len(status()['projects']) == 2
        left, right = query(first, 'return 17'), query(second, 'return 99')
        assert left['transport'] == 'local-pipe-v1' and right['snapshot_id'] != left['snapshot_id']
        assert query(first, 'return 17')['work_performed']['resident_cache_hit']
        denied = ce('query', 'quotaLimit', '--service', '--root', foreign, okay=False)
        assert denied['error']['code'] == 'service-project-denied', denied
        duplicate = subprocess.run([str(tool), 'serve', '--registered', '--home', str(home)],
            capture_output=True, encoding='utf-8', timeout=10)
        assert duplicate.returncode != 0 and status()['instance_id'] == instance

        print('RUN automatic uncommitted source refresh', flush=True)
        (first / 'quota.cpp').write_text('int quotaLimit() { return 8181; }\n', encoding='utf-8')
        updated = query(first, 'return 8181')
        assert updated['snapshot_id'] != left['snapshot_id']
        assert query(second, 'return 99')['snapshot_id'] == right['snapshot_id']
        read = ce('read', 'quota.cpp', '--snapshot', updated['snapshot_id'], '--root', first)
        assert read['native_read_complete']
        old = ce('read', 'quota.cpp', '--snapshot', left['snapshot_id'], '--root', first, okay=False)
        assert old['error']['code'] == 'source-snapshot-mismatch', old

        print('RUN real MCP protocol and pinned project', flush=True)
        mcp = Adapter(adapter_tool, first, home)
        try:
            hello = mcp.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                'clientInfo': {'name': 'shin-windows-product-test', 'version': '1'}})
            assert hello['result']['serverInfo']['name'] == 'shin'
            mcp.child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            mcp.child.stdin.flush()
            catalog = mcp.call('tools/list', {})['result']['tools']
            assert {tool['name'] for tool in catalog} == {'shin_status', 'shin_query', 'shin_inspect', 'shin_read', 'shin_help'}
            assert 'return 8181' in mcp.tool('shin_query', {'query': 'quotaLimit'})['source_context']
            assert not mcp.tool('shin_query', {'query': 'quotaLimit', 'root': str(second)})['ok']
            assert mcp.tool('shin_read', {'path': 'quota.cpp', 'snapshot': updated['snapshot_id']})['ok']
        finally:
            mcp.close()

        print('RUN MCP explicit multi-project navigation', flush=True)
        multi = Adapter(adapter_tool, first, home, roots=[first, second])
        try:
            multi.call('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                'clientInfo': {'name': 'shin-windows-multi-project-test', 'version': '1'}})
            multi.child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            multi.child.stdin.flush()
            projects = multi.tool('shin_projects', {})['projects']
            assert {Path(row['root']) for row in projects} == {first, second}
            handles = {Path(row['root']): row['project'] for row in projects}
            a, b = handles[first], handles[second]
            assert not multi.tool('shin_status', {})['ok']
            assert not multi.tool('shin_status', {'project': 'p' + '0' * 64})['ok']
            one = multi.send('tools/call', {'name': 'shin_query', 'arguments': {'project': a, 'query': 'quotaLimit'}})
            two = multi.send('tools/call', {'name': 'shin_query', 'arguments': {'project': b, 'query': 'quotaLimit'}})
            qb = json.loads(multi.response(two)['result']['content'][0]['text'])
            qa = json.loads(multi.response(one)['result']['content'][0]['text'])
            assert qa['mcp_project'] == a and qb['mcp_project'] == b
            assert 'return 8181' in qa['source_context'] and 'return 99' in qb['source_context']
            assert multi.tool('shin_read', {'project': a, 'path': 'quota.cpp', 'snapshot': qa['mcp_snapshot']})['ok']
            denied = multi.tool('shin_read', {'project': b, 'path': 'quota.cpp', 'snapshot': qa['mcp_snapshot']})
            assert denied['error']['code'] == 'mcp-snapshot-scope-mismatch'
        finally:
            multi.close()

        print('RUN MCP malformed input and unread output deadline', flush=True)
        for pressure in (False, True):
            raw = subprocess.Popen([str(adapter_tool), '--root', str(first), '--home', str(home)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                if pressure:
                    messages = [{'jsonrpc': '2.0', 'id': 0, 'method': 'initialize', 'params': {
                        'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'bounded-output', 'version': '1'}}},
                        {'jsonrpc': '2.0', 'method': 'notifications/initialized'}]
                    messages += [{'jsonrpc': '2.0', 'id': i, 'method': 'tools/list', 'params': {}} for i in range(1, 20)]
                    payload = ''.join(json.dumps(value) + '\n' for value in messages).encode()
                else:
                    payload = b'{"jsonrpc":"2.0","id":1,"id":2,"method":"ping"}\n'
                assert len(payload) < 4096
                raw.stdin.write(payload)
                raw.stdin.flush()
                raw.wait(timeout=8)
                assert raw.returncode == 0, ('MCP required forced shutdown', raw.returncode)
            finally:
                if raw.poll() is None:
                    raw.kill()
                    raw.wait(timeout=5)
                raw.stdin.close()
                raw.stdout.close()
        assert status()['instance_id'] == instance

        print('RUN persistent private history and concurrent restart', flush=True)
        def history():
            value = status()['telemetry']['history']
            assert value['status'] != 'unavailable', value
            return value if value['status'] == 'ready' and value['rows'] > 0 else None
        before = await_condition(history, 'numerical history', 15)
        assert all(entry['rss_bytes'] > 0 for entry in status()['resident_projects'])
        stop()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            restarts = list(pool.map(lambda _: ce('service-start'), range(2)))
        assert sum(bool(value['started']) for value in restarts) == 1
        assert restarts[0]['instance_id'] == restarts[1]['instance_id'] != instance
        assert ce('identity', '--root', first)['identity'] == initial_identity
        assert await_condition(history, 'reopened history', 15)['rows'] >= before['rows']
        query(first, 'return 8181')

        print('RUN unsafe history refusal without repair', flush=True)
        stop()
        database = home / 'service' / 'history' / 'telemetry.sqlite'
        original = hashlib.sha256(database.read_bytes()).hexdigest()
        journal = database.with_name(database.name + '-journal')
        journal.write_bytes(b'unsafe-fixture')
        private(journal, unsafe=True)
        ce('service-start')
        bad = await_condition(lambda: (value if (value := status()['telemetry']['history'])['status'] == 'unavailable' else None),
            'unsafe journal refusal', 15)
        assert bad['error_code']
        query(second, 'return 99')
        stop()
        assert journal.read_bytes() == b'unsafe-fixture'
        assert hashlib.sha256(database.read_bytes()).hexdigest() == original
        journal.unlink()
        ce('service-start')
        await_condition(history, 'history after removing fixture fault', 15)
        print('native Windows init/service/query/refresh/MCP/history passed', flush=True)
    finally:
        stop()


with tempfile.TemporaryDirectory(prefix='shin-service-win-') as directory:
    scenario(Path(directory).resolve())
