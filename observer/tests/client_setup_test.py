#!/usr/bin/env python3
"""First project from a stopped service; consume exported configs over real MCP."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import tomllib

ce, profiles = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()


def run(*args, ok=True):
    result = subprocess.run([str(ce), *map(str, args)], capture_output=True, text=True, timeout=120)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix='shin-setup-', dir='/tmp') as temporary:
    base = Path(temporary).resolve()
    root = base / 'project "quoted" café'
    root.mkdir()
    (root / 'limits.cpp').write_text('int previewLimit() { return 42; }\n')
    home = base / 'data'
    configs = {}
    for client in ['codex', 'opencode', 'rider', 'generic']:
        result = json.loads(run('mcp-config', '--client', client, '--root', root, '--home', home).stdout)
        assert result['ok'] and not result['client_modified'] and not result['readiness_assessed']
        text = run('mcp-config', '--client', client, '--root', root, '--home', home, '--text').stdout
        assert text == result['configuration']
        if client == 'codex':
            server = tomllib.loads(text)['mcp_servers']['shin']
        elif client == 'opencode':
            entry = json.loads(text)['mcp']['shin']
            assert entry['enabled'] and entry['type'] == 'local'
            server = {'command': entry['command'][0], 'args': entry['command'][1:]}
        else:
            server = json.loads(text)['mcpServers']['shin']
        assert server == result['server']
        assert server['args'] == ['--root', str(root), '--home', str(home)]
        configs[client] = server
    assert not home.exists(), 'configuration export must not create or enroll anything'
    run('mcp-config', '--client', 'unknown', '--root', root, '--home', home, ok=False)
    run('mcp-config', '--root', base / 'missing', '--home', home, ok=False)
    run('mcp-config', '--client', 'codex', '--client', 'rider', '--root', root, '--home', home, ok=False)
    try:
        # Same action as the first-run folder picker, with no existing registry/service.
        added = json.loads(run('project-add', '--root', root, '--home', home, '--profile-dir', profiles).stdout)
        assert added['ok'], added
        for client, server in configs.items():
            process = subprocess.Popen([server['command'], *server['args']], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=base, env={**os.environ, 'PATH': ''})
            pending = b''
            sequence = 0
            def rpc(method, params):
                global pending, sequence
                sequence += 1
                process.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': sequence, 'method': method, 'params': params}) + '\n').encode())
                process.stdin.flush()
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        response = json.loads(line)
                        if response.get('id') == sequence:
                            assert 'error' not in response, response
                            return response['result']
                    ready, _, _ = select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))
                    if ready:
                        part = os.read(process.stdout.fileno(), 65536)
                        assert part, process.stderr.read().decode()
                        pending += part
                raise AssertionError('MCP response timed out')
            def tool(name, arguments):
                result = rpc('tools/call', {'name': name, 'arguments': arguments})
                assert not result.get('isError', False), result
                value = json.loads(result['content'][0]['text'])
                assert value['ok'], value
                return value
            try:
                result = rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'first-run-test', 'version': '1'}})
                assert result['serverInfo']['name'] == 'shin'
                process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
                process.stdin.flush()
                assert 'shin_query' in [t['name'] for t in rpc('tools/list', {})['tools']]
                status = tool('shin_status', {})
                assert status['root'] == str(root) and status['local_ready']
                query = tool('shin_query', {'query': 'previewLimit'})
                read = tool('shin_read', {'path': 'limits.cpp', 'snapshot': query['snapshot_id']})
                assert read['native_read_complete'], read
            finally:
                process.terminate()
                try: process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.communicate(); raise
        assert list(root.iterdir()) == [root / 'limits.cpp'], 'setup changed the project'
    finally:
        run('service-stop', '--home', home)
print('First-run add, four parsed client formats, MCP status/query/read, and read-only export passed')
