#!/usr/bin/env python3
"""Native per-user install lifecycle. All state, links and services are isolated."""
import fcntl
import argparse
import hashlib
import json
import os
import select
from pathlib import Path
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('cmake')
parser.add_argument('build')
parser.add_argument('--archive', type=Path)
args = parser.parse_args()
cmake, build = args.cmake, args.build

with tempfile.TemporaryDirectory(prefix='shin-ins-', dir='/tmp') as temporary:
    work = Path(temporary).resolve()
    source = work / 'extracted kit'
    if args.archive:
        unpacked = work / 'unpacked'
        unpacked.mkdir()
        run = subprocess.run([cmake, '-E', 'tar', 'xf', str(args.archive.resolve())], cwd=unpacked,
                             capture_output=True, text=True, timeout=90)
        assert run.returncode == 0, run.stdout + run.stderr
        entries = list(unpacked.iterdir())
        assert len(entries) == 1 and entries[0].is_dir()
        entries[0].rename(source)
    else:
        run = subprocess.run([cmake, '--install', build, '--prefix', str(source)],
                             capture_output=True, text=True, timeout=90)
    assert run.returncode == 0, run.stdout + run.stderr
    tool = source / 'bin/shin-ce'
    user = work / 'user'
    user.mkdir(mode=0o700)
    env = {**os.environ, 'HOME': str(user), 'PATH': ''}
    prefix, bin_dir, data = user / 'opt/kit', user / 'commands', user / 'state'
    root = user / 'project'
    root.mkdir()
    (root / 'quota.cpp').write_text('int portableQuota() { return 17; }\n')
    command = ['install', '--prefix', str(prefix), '--bin-dir', str(bin_dir), '--home', str(data)]

    def call(*args, executable=tool, error=None):
        result = subprocess.run([str(executable), *map(str, args)], cwd=root, env=env,
                                capture_output=True, text=True, timeout=90)
        if error:
            assert result.returncode == 2, (args, result.stdout, result.stderr)
            payload = json.loads(result.stderr)
            assert payload['error']['code'] == error, (args, payload)
        else:
            assert result.returncode == 0 and not result.stderr, (args, result.stdout, result.stderr)
            payload = json.loads(result.stdout)
            assert payload['ok']
        return payload

    def status():
        return call('installation', '--prefix', prefix)

    def uninstall():
        return call('uninstall', '--prefix', prefix)

    def save_state(state):
        state_file.write_text(json.dumps(state))
        state_file.chmod(0o600)

    # Missing, extra, changed and linked source files fail before an install.
    extra = source / 'foreign.txt'
    extra.write_text('not a kit file')
    call(*command, error='installation-extra-file')
    extra.unlink()
    asset = source / 'share/shin-context/profiles/generic-source.json'
    original = asset.read_bytes()
    asset.write_bytes(original + b' ')
    call(*command, error='installation-file-changed')
    asset.write_bytes(original)
    alias = source / 'foreign-link'
    alias.symlink_to(asset)
    call(*command, error='installation-extra-file')
    alias.unlink()

    # Never adopt a pre-existing prefix or a pre-existing project data home.
    prefix.mkdir(parents=True)
    (prefix / 'keep').write_text('keep')
    call(*command, error='installation-not-owned')
    (prefix / 'keep').unlink()
    prefix.rmdir()
    data.mkdir(mode=0o700)
    (data / 'keep').write_text('keep')
    call(*command, error='installation-home-conflict')
    (data / 'keep').unlink()

    # Existing user commands survive refusal; retry resumes the recorded intent.
    bin_dir.mkdir(exist_ok=True)
    foreign = bin_dir / 'shin-ce'
    foreign.write_text('user-owned command')
    call(*command, error='installation-command-conflict')
    assert foreign.read_text() == 'user-owned command' and not prefix.exists()
    foreign.unlink()
    installed = call(*command)
    assert installed['schema_version'] == 'shin.local-install-response.v1' and not installed['readiness_assessed']
    assert installed['changed'] and not installed['client_settings_changed']
    assert not installed['autostart_installed']
    receipt = json.loads((prefix / '.shin-installation.json').read_text())
    state_file = Path(receipt['control']) / 'state.json'
    assert status()['integrity_verified']
    again = call(*command)
    assert not again['changed']
    assert (bin_dir / 'shin-ce').is_symlink()

    # Native operation lock fences mutations, without serializing read requests.
    with (Path(receipt['control']) / 'observer.lock').open('r+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        call('uninstall', '--prefix', prefix, error='writer-busy')
        call('init', executable=bin_dir / 'shin-ce', error='writer-busy')
    call('status', '--home', user / 'wrong-home', executable=bin_dir / 'shin-ce',
         error='installation-home-conflict')
    call('service-start', '--home', data, error='installation-home-conflict')
    assert not (user / 'wrong-home').exists()
    # Runtime uses the pinned data home even when caller HOME changes.
    alien = work / 'alien'
    alien.mkdir()
    changed_env = {**env, 'HOME': str(alien)}
    try:
        initialized = subprocess.run([str(bin_dir / 'shin-ce'), 'init'], cwd=root, env=changed_env,
                                     capture_output=True, text=True, timeout=90)
        assert initialized.returncode == 0, initialized.stdout + initialized.stderr
        query = call('query', 'portableQuota', executable=bin_dir / 'shin-ce')
        assert 'return 17' in query['source_context']
        assert not (alien / '.shin-ce').exists() and not (user / '.shin-ce').exists()
        wrong = subprocess.run([str(bin_dir / 'shin-mcp'), '--root', str(root), '--home', str(user / 'wrong-home')],
                               cwd=root, env=env, input='', capture_output=True, text=True, timeout=15)
        assert wrong.returncode == 2, (wrong.returncode, wrong.stdout, wrong.stderr)
        child = subprocess.Popen([str(bin_dir / 'shin-mcp'), '--root', str(root)], cwd=root, env=changed_env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            messages = [
                {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
                    'protocolVersion': '2024-11-05', 'capabilities': {},
                    'clientInfo': {'name': 'native-install-test', 'version': '1'}}},
                {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call', 'params': {
                    'name': 'shin_query', 'arguments': {'query': 'portableQuota'}}}]
            responses = {}
            for message in messages:
                child.stdin.write((json.dumps(message) + '\n').encode())
                child.stdin.flush()
                if 'id' not in message:
                    continue
                while message['id'] not in responses:
                    ready, _, _ = select.select([child.stdout], [], [], 15)
                    assert ready, 'MCP did not respond'
                    reply = json.loads(child.stdout.readline())
                    if 'id' in reply:
                        responses[reply['id']] = reply
            assert not responses[2]['result'].get('isError', False), responses
            assert 'return 17' in json.dumps(responses[2])
            child.stdin.close()
            child.stdin = None
            child.communicate(timeout=10)
            assert child.returncode == 0
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=10)
        verify = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(prefix / 'Shin Monitor.app')],
                                capture_output=True, text=True, timeout=15)
        assert verify.returncode == 0, verify.stderr

        # Uninstall refuses changed/unowned files before stopping a service or deleting payload.
        unexpected = prefix / 'personal-note'
        unexpected.write_text('keep me')
        call('uninstall', '--prefix', prefix, error='installation-extra-file')
        assert unexpected.read_text() == 'keep me'
        unexpected.unlink()
        target_asset = prefix / asset.relative_to(source)
        target_asset.write_bytes(original + b' ')
        call('uninstall', '--prefix', prefix, error='installation-file-changed')
        target_asset.write_bytes(original)
        # A second artifact, even with the same version, cannot overwrite this kit.
        manifest_file = source / 'shin-kit.json'
        original_manifest = manifest_file.read_bytes()
        manifest = json.loads(original_manifest)
        asset.write_bytes(original + b' ')
        for entry in manifest['files']:
            if entry['path'] == str(asset.relative_to(source)):
                entry['sha256'] = hashlib.sha256(asset.read_bytes()).hexdigest()
                entry['size'] = asset.stat().st_size
        manifest_file.write_text(json.dumps(manifest))
        call(*command, error='installation-replacement-refused')
        manifest_file.write_bytes(original_manifest)
        asset.write_bytes(original)
        registry = (data / 'registry/state.json').read_bytes()
        history = list((data / 'service/history').iterdir())
        assert history
        removed = uninstall()
        assert removed['data_preserved'] and not prefix.exists()
        assert not (bin_dir / 'shin-ce').is_symlink()
        assert (data / 'registry/state.json').read_bytes() == registry
        assert (root / 'quota.cpp').read_text() == 'int portableQuota() { return 17; }\n'
        assert list((data / 'service/history').iterdir())
        assert not (data / 'service/rpc.sock').exists()
        assert uninstall()['phase'] == 'removed'
    finally:
        if (data / 'service/rpc.sock').exists():
            subprocess.run([str(prefix / 'bin/shin-ce'), 'service-stop'], cwd=root, env=env,
                           capture_output=True, text=True, timeout=20)

    # Reinstall preserves the original data and index route.
    call(*command)
    assert (data / 'registry/state.json').read_bytes() == registry
    assert status()['integrity_verified']
    state = json.loads(state_file.read_text())
    # Model interruption after publication, before link completion/installed receipt.
    state['phase'] = 'installing'
    save_state(state)
    (bin_dir / 'shin-mcp').unlink()
    call('status', executable=bin_dir / 'shin-ce', error='installation-incomplete')
    call(*command)
    assert status()['integrity_verified']

    # Model interruption late in uninstall, with payload and receipt already absent.
    state = json.loads(state_file.read_text())
    state['phase'] = 'removing'
    save_state(state)
    (prefix / '.shin-installation.json').unlink()
    (prefix / 'bin/shin-ce').unlink()
    (prefix / 'shin-kit.json').unlink()
    uninstall()
    assert not prefix.exists() and data.exists()

    # Real process interruption during a copy must remain resumable.
    child = subprocess.Popen([str(tool), *command], cwd=root, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 20
    stage = Path(receipt['control']) / 'payload'
    interrupted = False
    while time.monotonic() < deadline and child.poll() is None:
        if (stage / 'bin/shin-ce').exists():
            child.send_signal(signal.SIGKILL)
            interrupted = True
            break
        time.sleep(0.001)
    out, err = child.communicate(timeout=20)
    assert interrupted, (out, err)
    call(*command)
    assert status()['integrity_verified']
    uninstall()
    assert not stage.exists()
    # Interrupted staging allocation and the home claim's atomic journal resume.
    state = json.loads(state_file.read_text())
    state['phase'] = 'installing'
    state['receipt']['prefix_identity'] = ''
    save_state(state)
    stage.mkdir(mode=0o700)
    claim_file = data / 'installation/state.json'
    claim_file.with_name('state.pending').write_text('{"interrupted":')
    claim_file.with_name('state.pending').chmod(0o600)
    call(*command)
    assert status()['integrity_verified']

    # A foreground service releases its startup fence and remains stoppable.
    foreground = subprocess.Popen([str(bin_dir / 'shin-ce'), 'serve', '--registered'], cwd=root, env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 20
        while not (data / 'service/rpc.sock').exists() and foreground.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        assert (data / 'service/rpc.sock').exists()
        uninstall()
        out, err = foreground.communicate(timeout=15)
        assert foreground.returncode == 0, (out, err)
    finally:
        if foreground.poll() is None:
            foreground.kill()
            foreground.communicate(timeout=10)

    # Custom prefix overlaps the unused default home, not its actual data home.
    unusual = user / '.shin-ce/kit'
    unusual_args = ['install', '--prefix', str(unusual), '--bin-dir', str(user / 'other-bin'), '--home', str(user / 'other-data')]
    call(*unusual_args)
    assert call('installation', '--prefix', unusual)['integrity_verified']
    call('uninstall', '--prefix', unusual)
    fresh = work / 'fresh-user'
    fresh.mkdir(mode=0o700)
    env['HOME'] = str(fresh)
    fresh_claim = fresh / '.shin-ce/installation'
    fresh_claim.mkdir(mode=0o700, parents=True)
    (fresh / '.shin-ce').chmod(0o700)
    (fresh_claim / 'state.pending').write_text('{"interrupted":')
    (fresh_claim / 'state.pending').chmod(0o600)
    simple = call('install')
    assert simple['prefix'] == str(fresh / '.local/opt/shin-local')
    assert simple['home'] == str(fresh / '.shin-ce')
    assert call('installation')['integrity_verified']
    call('uninstall')
    print(json.dumps({'ok': True, 'native_installation': True, 'data_preserved': True,
                      'copy_interruption_resumed': True, 'home_routing': True,
                      'archive_verified': bool(args.archive),
                      'client_settings_changed': False, 'live_installation_changed': False}))
