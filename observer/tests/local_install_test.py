#!/usr/bin/env python3
"""Relocate an isolated install and exercise shin-ce with no shell helpers."""
import json
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import platform
import plistlib
import select
import subprocess
import sys
import tempfile
import zipfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('cmake')
parser.add_argument('build')
parser.add_argument('complete', nargs='?', choices=['0', '1'], default='0')
parser.add_argument('--archive', type=Path)
args = parser.parse_args()
cmake, build, complete_kit = args.cmake, args.build, args.complete == '1'


def verify_macos_dependencies(prefix):
    programs = [prefix / 'bin' / name for name in
                ['shin-context', 'shin-observer', 'shin-ce', 'shin-mcp', 'shin-usage']]
    app = prefix / 'Shin Monitor.app'
    version = json.loads(subprocess.run([str(prefix / 'bin/shin-ce'), '--version'],
        check=True, capture_output=True, text=True, timeout=10).stdout)['version']
    with (app / 'Contents/Info.plist').open('rb') as source:
        metadata = plistlib.load(source)
    assert metadata['CFBundleShortVersionString'] == version.split('-', 1)[0], (metadata, version)
    assert metadata['CFBundleVersion'].isdigit(), metadata
    programs.append(app / 'Contents/MacOS/Shin Monitor')
    images = programs + list(prefix.rglob('*.dylib'))
    for path in images:
        output = subprocess.run(['/usr/bin/otool', '-L', str(path)], check=True,
                                capture_output=True, text=True, timeout=10).stdout
        for line in output.splitlines()[1:]:
            dependency = line.strip().rsplit(' (compatibility version ', 1)[0]
            if dependency.startswith(('/System/Library/', '/usr/lib/')):
                continue
            assert dependency.startswith(('@executable_path/', '@loader_path/')), (path, dependency)
            # The kit's dependency libraries share each executable's directory.
            resolved = Path(dependency.replace('@executable_path', str(path.parent))
                            .replace('@loader_path', str(path.parent))).resolve()
            assert resolved.is_relative_to(prefix) and resolved.is_file(), (path, dependency)
        signed = subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)],
                                capture_output=True, text=True, timeout=10)
        assert signed.returncode == 0, signed.stdout + signed.stderr
    signed = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)],
                            capture_output=True, text=True, timeout=10)
    assert signed.returncode == 0, signed.stdout + signed.stderr


with tempfile.TemporaryDirectory(prefix="shin-ce-install-", dir='/tmp') as temporary, ExitStack() as cleanup:
    work = Path(temporary).resolve()
    prefix = work / "installed"
    if args.archive:
        with zipfile.ZipFile(args.archive) as archive:
            for entry in archive.infolist():
                path = Path(entry.filename)
                assert not path.is_absolute() and '..' not in path.parts
                assert not any(part.startswith('._') or part == '__MACOSX' for part in path.parts)
        (work / 'unpacked').mkdir()
        subprocess.run([cmake, '-E', 'tar', 'xf', str(args.archive.resolve())], cwd=work / 'unpacked',
                       check=True, capture_output=True, text=True, timeout=30)
        entries = list((work / 'unpacked').iterdir())
        assert len(entries) == 1 and entries[0].is_dir()
        entries[0].rename(prefix)
    else:
        installed = subprocess.run([cmake, "--install", build, "--prefix", str(prefix)],
                                   capture_output=True, text=True, timeout=60)
        assert installed.returncode == 0, installed.stdout + installed.stderr
    relocated = work / "relocated kit"
    prefix.rename(relocated)
    if complete_kit:
        assert platform.system() == 'Darwin'
        verify_macos_dependencies(relocated)
        for asset in ['share/shin-context/profiles/generic-retool.json',
                      'share/shin-context/contracts/agent-readiness-v1.json',
                      'share/shin-context/codex/skills/shin-context/SKILL.md',
                      'share/shin-observer/licenses/openssl/LICENSE.txt']:
            assert (relocated / asset).is_file(), asset
    tool = relocated / "bin/shin-ce"
    root = work / "project"
    root.mkdir()
    (root / "README.md").write_text("# Portable quota\n\nThe quota is defined by quotaLimit.\n")
    (root / "quota.cpp").write_text("int quotaLimit() { return 17; }\n")
    home = work / "home"
    home.mkdir(mode=0o700)
    env = {**os.environ, "HOME": str(home), "PATH": ""}
    if complete_kit:
        for name in ['shin-context', 'shin-observer', 'shin-ce', 'shin-usage']:
            run = subprocess.run([str(relocated / 'bin' / name), '--version'], cwd=root, env=env,
                                 capture_output=True, text=True, timeout=10)
            assert run.returncode == 0, (name, run.stdout, run.stderr)
    def stop_service():
        endpoint = home / '.shin-ce/service/rpc.sock'
        if endpoint.exists():
            stopped = subprocess.run([str(tool), 'service-stop'], cwd=root, env=env,
                                     capture_output=True, text=True, timeout=15)
            assert stopped.returncode == 0, stopped.stdout + stopped.stderr
            deadline = time.monotonic() + 10
            while endpoint.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not endpoint.exists(), 'relocated service failed to stop'
    cleanup.callback(stop_service)
    results = []
    for command in [["init"], ["status", "--service"], ["query", "quotaLimit", "--service"]]:
        run = subprocess.run([str(tool), *command], cwd=root, env=env,
                             capture_output=True, text=True, timeout=45)
        assert run.returncode == 0 and not run.stderr, run.stdout + run.stderr
        result = json.loads(run.stdout)
        assert result["ok"] and result["local_ready"] and not result["canonical_ready"]
        assert not result["delivery_performed"] and not result["consumer_mutated"]
        results.append(result)
    assert results[0]['daemon_started'] and results[0]['service_enrolled']
    assert "return 17" in results[-1]["source_context"]
    snapshot = results[-1]["snapshot_id"]
    for command in [["inspect", "quota.cpp"], ["read", "quota.cpp"]]:
        run = subprocess.run([str(tool), *command, "--snapshot", snapshot, '--service'], cwd=root, env=env,
                             capture_output=True, text=True, timeout=45)
        assert run.returncode == 0 and not run.stderr, run.stdout + run.stderr
        result = json.loads(run.stdout)
        assert result["snapshot_id"] == snapshot and result["local_ready"]
        if command[0] == "read":
            assert "return 17" in result["source_context"]
    assert set(path.name for path in root.iterdir()) == {"README.md", "quota.cpp"}
    assert (home / ".shin-ce").is_dir()
    if complete_kit:
        messages = [
            {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
                'protocolVersion': '2024-11-05', 'capabilities': {},
                'clientInfo': {'name': 'relocation-test', 'version': '1'}}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list', 'params': {}},
            {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': {
                'name': 'shin_query', 'arguments': {'query': 'quotaLimit'}}}]
        child = subprocess.Popen([str(relocated / 'bin/shin-mcp'), '--root', str(root)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=root, env=env)
        def stop_mcp():
            if child.poll() is None:
                child.terminate()
            try:
                child.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                raise AssertionError('relocated MCP did not stop')
        cleanup.callback(stop_mcp)
        responses, pending = {}, b''
        for message in messages:
            child.stdin.write((json.dumps(message) + '\n').encode())
            child.stdin.flush()
            if 'id' not in message:
                continue
            deadline = time.monotonic() + 15
            while message['id'] not in responses:
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    response = json.loads(line)
                    if 'id' in response:
                        responses[response['id']] = response
                if message['id'] in responses:
                    break
                assert time.monotonic() < deadline, responses
                ready, _, _ = select.select([child.stdout], [], [], max(0, deadline - time.monotonic()))
                if ready:
                    part = os.read(child.stdout.fileno(), 8192)
                    assert part, 'MCP ended before replying'
                    pending += part
        assert {tool['name'] for tool in responses[2]['result']['tools']} == {
            'shin_status', 'shin_query', 'shin_inspect', 'shin_read', 'shin_help'}
        assert not responses[3]['result'].get('isError', False), responses[3]
        assert 'return 17' in json.dumps(responses[3]), responses[3]
        child.stdin.close()
        child.stdin = None
        child.communicate(timeout=10)
        assert child.returncode == 0
    for command in ['detach', 'init']:
        run = subprocess.run([str(tool), command], cwd=root, env=env,
                             capture_output=True, text=True, timeout=45)
        assert run.returncode == 0, run.stdout + run.stderr
        result = json.loads(run.stdout)
        if command == 'detach':
            assert result['state'] == 'detached' and result['data_preserved']
        else:
            assert result['service_enrolled'] and result['local_ready']
    print(json.dumps({"schema_version": "shin.ce-install-check.v1", "ok": True,
                      "complete_kit": complete_kit, "external_runtime_dependencies": not complete_kit,
                      "archive_verified": bool(args.archive),
                      "relocated": True, "shell_tools_required": False, "commands": ["init", "status", "query", "inspect", "read", "detach", "init"],
                      "core_version": results[0]["core_version"], "version": results[0]["version"]}))
