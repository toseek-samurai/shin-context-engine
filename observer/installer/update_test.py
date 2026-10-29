#!/usr/bin/env python3
"""Exercise native update, legacy adoption and crash recovery in disposable homes.

Older release identities are synthetic manifest fixtures; executable compatibility
with a real previous release is additionally checked during the owner rollout.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from integration_test import Fixture

parser = argparse.ArgumentParser()
parser.add_argument('cmake')
parser.add_argument('build')
parser.add_argument('--archive', type=Path)
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix='shin-up-', dir='/tmp') as temporary:
    work = Path(temporary).resolve()
    source = work / 'new kit'
    if args.archive:
        unpacked = work / 'archive'
        unpacked.mkdir()
        subprocess.run([args.cmake, '-E', 'tar', 'xf', str(args.archive.resolve())], cwd=unpacked,
                       check=True, capture_output=True, timeout=90)
        entries = list(unpacked.iterdir())
        assert len(entries) == 1 and entries[0].is_dir()
        entries[0].rename(source)
    else:
        subprocess.run([args.cmake, '--install', args.build, '--prefix', str(source)],
                       check=True, capture_output=True, timeout=90)
    tool = source / 'bin/shin-ce'
    user = work / 'user'
    user.mkdir(mode=0o700)
    prefix, bin_dir, home = user / 'kit', user / 'bin', user / 'data'
    bin_dir.mkdir()
    root = user / 'project'
    root.mkdir()
    (root / 'quota.cpp').write_text('int updateQuota() { return 41; }\n')
    env = {**os.environ, 'HOME': str(user), 'PATH': ''}
    shutil.copytree(source, prefix)
    names = ['shin-ce', 'shin-context', 'shin-observer', 'shin-mcp', 'shin-usage']
    for name in names:
        (bin_dir / name).symlink_to(prefix / 'bin' / name)
    manifest = json.loads((source / 'shin-kit.json').read_text())
    asset_name = next(f['path'] for f in manifest['files'] if f['path'].endswith('/local-kit.md'))
    asset = source / asset_name
    old_versions = iter(['2.0.5-dev.0', '2.0.5-dev.1', '2.0.5-dev.2'])

    def write_json(path, value, private=False):
        path.write_text(json.dumps(value))
        if private:
            path.chmod(0o600)

    def previous_fixture(owned=False, version=None, core_version=None):
        value = json.loads((prefix / 'shin-kit.json').read_text())
        value['version'] = version or next(old_versions)
        if core_version is not None:
            value['core_version'] = core_version
        old_asset = prefix / asset_name
        old_asset.write_text(old_asset.read_text() + '\nPrevious release fixture.\n')
        for row in value['files']:
            if row['path'] == asset_name:
                row.update(sha256=hashlib.sha256(old_asset.read_bytes()).hexdigest(), size=old_asset.stat().st_size)
        write_json(prefix / 'shin-kit.json', value)
        if owned:
            state = json.loads(state_file.read_text())
            receipt = state['receipt']
            receipt['manifest_sha256'] = hashlib.sha256((prefix / 'shin-kit.json').read_bytes()).hexdigest()
            write_json(prefix / '.shin-installation.json', receipt, True)
            state['kit'] = value
            write_json(state_file, state, True)

    def call(*words, executable=tool, error=None, progress=None, language=None):
        if progress:
            words = (*words, "--progress-json" if progress == "json" else "--progress")
        call_env = {**env, **({"LC_ALL": language} if language else {})}
        result = subprocess.run([str(executable), *map(str, words)], cwd=root, env=call_env,
                                capture_output=True, text=True, timeout=90)
        if error:
            assert result.returncode == 2, (words, result.stdout, result.stderr)
            receipt_lines, observed = [], []
            for line in result.stderr.splitlines(keepends=True):
                try:
                    row = json.loads(line)
                except ValueError:
                    row = None
                if progress == 'json' and isinstance(row, dict) and set(row) == {'step', 'received', 'total'}:
                    observed.append(row['step'])
                else:
                    receipt_lines.append(line)
            value = json.loads(''.join(receipt_lines))
            assert value['error']['code'] == error, value
            if progress == 'json':
                assert 16 in observed and observed[-1] == 18 and 17 not in observed, observed
        else:
            assert result.returncode == 0, (words, result.stdout, result.stderr)
            if progress == 'json':
                frames = [json.loads(line) for line in result.stderr.splitlines()]
                assert frames and all(set(row) == {'step', 'received', 'total'} for row in frames)
                stages = [row['step'] for row in frames]
                assert stages[0] == 0 and stages[-1] == 17, stages
                if '--adopt-existing' in words:
                    for step in (5, 9, 10, 11, 12, 7):
                        assert step in stages, (step, stages)
                    assert any(row['step'] == 9 and row['received'] == row['total'] == 2 for row in frames), frames
                    assert any(row['step'] == 10 and row['received'] == row['total'] > 0 for row in frames), frames
            elif progress:
                assert ('Обновление завершено' if language else 'Update completed') in result.stderr, result.stderr
                assert '\x1b' not in result.stderr, 'redirected human progress contains terminal controls'
            else:
                assert not result.stderr, result.stderr
            value = json.loads(result.stdout)
            assert value['ok'], value
        return value

    def stop_service():
        call('service-stop', '--home', home, executable=prefix / 'bin/shin-ce')
        until = time.monotonic() + 15
        while (home / 'service/rpc.sock').exists():
            assert time.monotonic() < until, 'service did not release its endpoint'
            time.sleep(0.01)

    managed = os.environ.get('SHIN_TEST_LAUNCHD') == '1'
    update = ['update', '--prefix', str(prefix)]
    adoption = [*update, '--adopt-existing', '--home', str(home), '--bin-dir', str(bin_dir)]
    previous_fixture()
    try:
        call('init', '--home', home, executable=prefix / 'bin/shin-ce')
        call('query', 'updateQuota', '--home', home, executable=prefix / 'bin/shin-ce')
        quality = Fixture(work, 'quality-canonical')
        quality.write_source('# Contract\n\nThe retry ceiling belongs to readLimit.\n')
        (quality.root / 'limits.js').write_text('export function readLimit() { return 17; }\n')
        profile = json.loads(quality.profile_path.read_text())
        profile.update(bootstrapOnly=False, sourceFactRoots=['.'], sourceFactExtensions=['.js'],
            evalCases=[{'id': 'contract', 'query': 'retry ceiling', 'mustPaths': ['README.md']}],
            packEvalCases=[{'id': 'contract-pack', 'mustPaths': ['README.md'], 'expectedComplete': True,
                'request': {'schema_version': 'shin.task-request.v1', 'request_id': 'contract-pack',
                    'task': {'objective': 'Find the retry ceiling', 'must': [{'id': 'rule', 'query': 'retry ceiling'}]},
                    'budget': {'max_evidence_bytes': 8192, 'max_records': 4,
                               'per_query_records': 4, 'per_document_records': 2}}}])
        quality.profile_path.write_text(json.dumps(profile))
        call('--root', quality.root, '--profile', 'observer-fixture', '--profile-dir', quality.profiles,
             '--out-dir', quality.out, 'build', executable=prefix / 'bin/shin-context')
        call('sample', '--config', quality.config, executable=prefix / 'bin/shin-observer')
        call('init', '--root', quality.root, '--home', home, '--observer-config', quality.config,
             '--manage-index', executable=prefix / 'bin/shin-ce')
        if managed:
            call('service-install', '--home', home, executable=prefix / 'bin/shin-ce')
        registry = (home / 'registry/state.json').read_bytes()
        identity = (home / 'identity/state.json').read_bytes()
        history = home / 'service/history/telemetry.sqlite'
        history_inode = history.stat().st_ino
        with sqlite3.connect(f'file:{history}?mode=ro', uri=True) as db:
            events_before = db.execute('SELECT COUNT(*) FROM events').fetchone()[0]
        # Fixture manifests differ from the current binaries: version-pinned RPC
        # is covered with a real previous release, not by lying about a live service.
        stop_service()
        call(*update, error='installation-not-owned')
        assert not (prefix / '.shin-installation.json').exists()
        assert not (home / 'installation').exists()
        call(*adoption[:-2], error='invalid-arguments')
        extra = prefix / 'personal-file'
        extra.write_text('preserve')
        call(*adoption, error='installation-extra-file')
        assert extra.read_text() == 'preserve'
        extra.unlink()
        # A current, safe project with a search warning must not block adoption.
        # Stale publications during a Core transition are exercised below.
        quality.write_source('# Other\n\nNo relevant evidence here.\n')
        call('watch', '--root', quality.root, '--home', home, '--samples', '2', executable=prefix / 'bin/shin-ce')
        migrated = call(*adoption, progress="json")
        assert migrated['changed'] and migrated['data_preserved'] and migrated['service_restored']
        receipt = json.loads((prefix / '.shin-installation.json').read_text())
        state_file = Path(receipt['control']) / 'state.json'
        stage = Path(receipt['control']) / 'update-payload'
        assert call('installation', '--prefix', prefix)['integrity_verified']
        assert not (home / 'service/rpc.sock').exists()
        call('service-start', executable=prefix / 'bin/shin-ce')
        available = call('query', 'readLimit', '--root', quality.root, '--home', home, executable=prefix / 'bin/shin-ce')
        assert available['local_ready'] and not available['canonical_ready']
        assert available['quality_warning']['state'] == 'degraded'
        assert 'return 17;' in available['source_context']
        assert not call(*update, progress='text')['changed']
        assert not call(*update, progress='text', language='ru_RU.UTF-8')['changed']
        assert (prefix / asset_name).read_bytes() == asset.read_bytes()
        call('update', executable=prefix / 'bin/shin-ce', error='installation-update-source')
        with (Path(receipt['control']) / 'operation/observer.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            call(*update, error='writer-busy')
            call('uninstall', '--prefix', prefix, error='writer-busy')

        # Newer-version admission precedes replacement; modified same-version kit fails.
        pristine = (source / 'shin-kit.json').read_bytes()
        source_manifest = json.loads(pristine)
        asset_bytes = asset.read_bytes()
        asset.write_bytes(asset_bytes + b'\nChanged release fixture.\n')
        for row in source_manifest['files']:
            if row['path'] == asset_name:
                row.update(sha256=hashlib.sha256(asset.read_bytes()).hexdigest(), size=asset.stat().st_size)
        write_json(source / 'shin-kit.json', source_manifest)
        call(*update, error='installation-version-not-newer')
        (source / 'shin-kit.json').write_bytes(pristine)
        asset.write_bytes(asset_bytes)

        # Real process interruption while staging. The old payload remains intact.
        stop_service()
        previous_fixture(owned=True, core_version='unsupported-core')
        call(*update, error='installation-core-transition-unsupported')
        # Existing format-preserving previews migrate without changing authority.
        binding_path = home / 'projects' / hashlib.sha256(str(root).encode()).hexdigest() / 'state.json'
        for previous_core in ('2.0.5-dev.6',):
            previous_fixture(owned=True, version='2.0.5-dev.15', core_version=previous_core)
            old_patch_binding = json.loads(binding_path.read_text())
            old_patch_binding['core_version'] = previous_core
            write_json(binding_path, old_patch_binding, True)
            assert call(*update)['changed']
            current_binding = json.loads(binding_path.read_text())
            assert current_binding['core_version'] == manifest['core_version']
            for key in old_patch_binding.keys() - {'core_version', 'prepared_snapshot'}:
                assert current_binding[key] == old_patch_binding[key], key
        # Future/reverse Core transitions are refused before touching bindings.
        previous_fixture(owned=True, version='2.0.5-dev.15', core_version='2.0.5-dev.999')
        binding_before_refusal = binding_path.read_bytes()
        call(*update, error='installation-core-transition-unsupported')
        assert binding_path.read_bytes() == binding_before_refusal
        # This explicit format-preserving Core patch is admitted; unknown Core
        # identities remain refused above, including before any payload swap.
        previous_fixture(owned=True, version='2.0.5-dev.1', core_version='2.0.5-dev.1')
        # New engine recovery must refresh the stale publication using its grant;
        # pre-admission must not demand that the old engine repair itself first.
        (root / 'quota.cpp').write_text('int updateQuota() { return 42; }\n')
        quality.write_source('# Changed again\n\nStill no relevant evidence.\n')
        binding_path = home / 'projects' / hashlib.sha256(str(root).encode()).hexdigest() / 'state.json'
        old_binding = json.loads(binding_path.read_text())
        old_binding['core_version'] = '2.0.5-dev.1'
        write_json(binding_path, old_binding, True)
        previous_manifest = (prefix / 'shin-kit.json').read_bytes()
        child = subprocess.Popen([str(tool), *update], cwd=root, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        killed = False
        until = time.monotonic() + 20
        while child.poll() is None and time.monotonic() < until:
            state = json.loads(state_file.read_text())
            if state.get('update', {}).get('step') == 'copying' and stage.exists() and len(list(stage.rglob('*'))) > 12:
                child.kill()
                killed = True
                break
            time.sleep(0.001)
        out, err = child.communicate(timeout=20)
        assert killed, (out, err)
        pending_recovery = json.loads(state_file.read_text())['update']
        assert pending_recovery['recover_publications'] is True
        assert pending_recovery['runtime']['unready_roots'] == []
        assert pending_recovery['runtime']['running'] is False
        assert [row['root'] for row in pending_recovery['bootstrap_migrations']] == [str(root)]
        assert (prefix / 'shin-kit.json').read_bytes() == previous_manifest
        assert (home / 'registry/state.json').read_bytes() == registry
        recovery_child = subprocess.Popen([str(tool), *update], cwd=root, env=env,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        interrupted_recovery = False
        until = time.monotonic() + 25
        while recovery_child.poll() is None and time.monotonic() < until:
            journal = json.loads(state_file.read_text())
            if journal['phase'] == 'activating' and journal['update']['recover_publications']:
                recovery_child.kill()
                interrupted_recovery = True
                break
            time.sleep(0.001)
        out, err = recovery_child.communicate(timeout=20)
        assert interrupted_recovery, (out, err)
        assert json.loads(state_file.read_text())['phase'] == 'activating'
        resumed = call(*update)
        assert resumed['changed'] and resumed['data_preserved']
        call('service-start', executable=prefix / 'bin/shin-ce')
        recovered_query = call('query', 'updateQuota', executable=prefix / 'bin/shin-ce')
        assert recovered_query['local_ready'] and 'return 42;' in recovered_query['source_context']
        migrated_binding = json.loads(binding_path.read_text())
        assert migrated_binding['core_version'] == manifest['core_version']
        for key in old_binding.keys() - {'core_version', 'prepared_snapshot'}:
            assert migrated_binding[key] == old_binding[key], key

        # Model the atomic rename committing just before the journal acknowledgement.
        # Both real payloads/identities are those produced by the successful update.
        stop_service()
        completed = json.loads(state_file.read_text())
        target_receipt, target_kit = completed['receipt'], completed['kit']
        old = completed['previous']
        journal = {'receipt': target_receipt, 'kit': target_kit, 'old_receipt': old['receipt'],
                   'old_kit': old['kit'], 'runtime': {'managed': managed, 'running': True},
                   'registry_sha256': hashlib.sha256(registry).hexdigest(), 'step': 'swapping'}
        pending = {**completed, 'phase': 'updating', 'receipt': old['receipt'], 'kit': old['kit'], 'update': journal}
        pending.pop('previous')
        write_json(state_file, pending, True)
        assert call(*update)['changed']
        assert call('query', 'updateQuota', executable=prefix / 'bin/shin-ce')['local_ready']

        # A damaged newly switched payload is rolled back before reporting success.
        stop_service()
        completed = json.loads(state_file.read_text())
        pending = {**completed, 'phase': 'activating', 'update': {**journal, 'step': 'swapping'}}
        pending.pop('previous')
        write_json(state_file, pending, True)
        (prefix / 'bin/shin-context').chmod(0o600)
        call(*update, error='installation-update-rolled-back', progress='json')
        assert call('installation', '--prefix', prefix)['version'] == old['kit']['version']
        assert call('query', 'updateQuota', executable=prefix / 'bin/shin-ce')['local_ready']
        assert (home / 'registry/state.json').read_bytes() == registry
        assert (home / 'identity/state.json').read_bytes() == identity
        assert history.stat().st_ino == history_inode
        with sqlite3.connect(f'file:{history}?mode=ro', uri=True) as db:
            assert db.execute('PRAGMA quick_check').fetchone()[0] == 'ok'
            assert db.execute('SELECT COUNT(*) FROM events').fetchone()[0] >= events_before
        stop_service()
        # Retained corruption is never silently deleted, even from a rollback kit.
        call(*update, error='installation-file-changed')
        (stage / 'bin/shin-context').chmod(0o755)
        assert call(*update)['changed']
        assert call('installation', '--prefix', prefix)['version'] == manifest['version']

        # A source edit and maintenance lock can arrive after candidate admission.
        # Model that activation boundary with the verified payloads retained above.
        completed = json.loads(state_file.read_text())
        if (home / 'service/rpc.sock').exists():
            stop_service()
        pending = {**completed, 'phase': 'activating', 'update': {
            'receipt': completed['receipt'], 'kit': completed['kit'],
            'old_receipt': completed['previous']['receipt'], 'old_kit': completed['previous']['kit'],
            'runtime': {'managed': managed, 'running': True},
            'registry_sha256': hashlib.sha256(registry).hexdigest(), 'step': 'swapping'}}
        pending.pop('previous')
        write_json(state_file, pending, True)
        with (binding_path.parent / 'observer.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            (root / 'quota.cpp').write_text('int updateQuota() { return 43; }\n')
            rejected = call(*update, error='installation-update-rolled-back')
            diagnostic = rejected['error']['diagnostic']
            assert diagnostic['schema_version'] == 'shin.validation-diagnostic.v1'
            assert any(code in json.dumps(diagnostic) for code in ['project-busy', 'writer-busy']), diagnostic
            assert str(root) not in json.dumps(diagnostic)
            restored = json.loads(state_file.read_text())
            assert 'resume_runtime' not in restored and 'update' not in restored
            assert restored['last_rollback']['service_restored']
            assert not restored['last_rollback']['all_projects_ready']
            assert restored['last_rollback']['pending_projects'] == 1
            receipt_status = call('installation', '--prefix', prefix)
            assert not receipt_status['recovery_pending']
            assert receipt_status['last_rollback']['pending_projects'] == 1
            assert receipt_status['last_update_diagnostic'] == diagnostic
            live = call('service-status', '--home', home, executable=prefix / 'bin/shin-ce')
            assert live['service_running'] and live['enrollment_errors']
            assert (home / 'registry/state.json').read_bytes() == registry
            assert (home / 'identity/state.json').read_bytes() == identity
            assert history.stat().st_ino == history_inode
        # Ordinary background refresh, rather than the updater, repairs freshness.
        until = time.monotonic() + 35
        while True:
            result = subprocess.run([str(prefix / 'bin/shin-ce'), 'query', 'updateQuota', '--home', str(home)],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=30)
            if result.returncode == 0 and 'return 43;' in json.loads(result.stdout).get('source_context', ''):
                break
            assert time.monotonic() < until, (result.stdout, result.stderr)
            time.sleep(0.2)
        stop_service()

        # A hard engine/identity refusal is not a transient readiness condition.
        # Preserve the recovery journal until the test restores its own binding.
        assert call(*update)['changed']
        completed = json.loads(state_file.read_text())
        pending = {**completed, 'phase': 'activating', 'update': {
            'receipt': completed['receipt'], 'kit': completed['kit'],
            'old_receipt': completed['previous']['receipt'], 'old_kit': completed['previous']['kit'],
            'runtime': {'managed': managed, 'running': True},
            'registry_sha256': hashlib.sha256(registry).hexdigest(), 'step': 'swapping'}}
        pending.pop('previous')
        write_json(state_file, pending, True)
        binding_bytes = binding_path.read_bytes()
        unsupported = json.loads(binding_bytes)
        unsupported['core_version'] = 'unsupported-core'
        write_json(binding_path, unsupported, True)
        try:
            rejected = call(*update, error='installation-update-recovery-required')
            assert 'refresh-engine-changed' in json.dumps(rejected['error']['diagnostic']), rejected
            assert 'resume_runtime' in json.loads(state_file.read_text())
            # Explicit legacy review cannot waive this Core transition.
            call(*update, '--accept-degraded-rollback', error='installation-recovery-review-not-applicable')
            assert 'resume_runtime' in json.loads(state_file.read_text())
        finally:
            binding_path.write_bytes(binding_bytes)
        until = time.monotonic() + 35
        while call('service-status', '--home', home, executable=prefix / 'bin/shin-ce')['enrollment_errors']:
            assert time.monotonic() < until, 'Restored fixture binding did not readmit normally'
            time.sleep(0.2)
        call(*update, error='installation-update-rolled-back')
        assert not call('installation', '--prefix', prefix)['recovery_pending']
        call('uninstall', '--prefix', prefix)
        assert not prefix.exists() and not stage.exists() and history.exists()
        assert (home / 'registry/state.json').read_bytes() == registry
        print(json.dumps({'ok': True, 'legacy_adoption': True, 'native_update': True,
                          'interrupted_copy_resumed': True, 'atomic_switch_resumed': True,
                          'failed_activation_rolled_back': True, 'history_preserved': True,
                          'managed_service': managed, 'live_installation_changed': False}))
    finally:
        if managed and (home / 'supervision/state.json').exists():
            supervision = json.loads((home / 'supervision/state.json').read_text())
            agent = Path(supervision['agent_path'])
            assert supervision['definition']['unit']['Program'] == str(prefix / 'bin/shin-ce')
            subprocess.run(['/bin/launchctl', 'bootout', f"gui/{os.getuid()}/{supervision['label']}"],
                           capture_output=True, timeout=20)
            if agent.exists():
                assert agent.read_text() == supervision['definition']['plist']
                agent.unlink()
        if prefix.exists() and (home / 'service/rpc.sock').exists():
            subprocess.run([str(prefix / 'bin/shin-ce'), 'service-stop', '--home', str(home)],
                           env=env, capture_output=True, timeout=20)
