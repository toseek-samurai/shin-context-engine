#!/usr/bin/env python3
"""Verify native auto-update recovery with real previous/candidate kits.

Runs only in a disposable home. Does not fetch releases or alter a live kit.
The native child entry point is the one invoked by the preview update helper.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from integration_test import Fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--previous-kit', type=Path, required=True)
parser.add_argument('--candidate-kit', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
parser.add_argument('--expected-error', choices=['installation-recovery-command'])
args = parser.parse_args()
evidence = {'network_delivery_tested': False, 'live_installation_changed': False}

with tempfile.TemporaryDirectory(prefix='shin-previous-update-', dir='/tmp') as temporary:
    work = Path(temporary).resolve()
    user = work / 'user'
    user.mkdir(mode=0o700)
    prefix, home, commands = user / 'kit', user / 'data', user / 'bin'
    old = work / 'previous'
    shutil.copytree(args.previous_kit.resolve(), old, ignore=shutil.ignore_patterns('.shin-installation.json'))
    candidate = work / 'candidate'
    shutil.copytree(args.candidate_kit.resolve(), candidate, ignore=shutil.ignore_patterns('.shin-installation.json'))
    previous_manifest = json.loads((old / 'shin-kit.json').read_text())
    next_manifest = json.loads((candidate / 'shin-kit.json').read_text())
    if previous_manifest['core_version'] != '2.0.5-dev.3' or next_manifest['core_version'] != '2.0.5-dev.4':
        parser.error('This regression requires a real Core dev.3 previous kit and Core dev.4 candidate.')
    evidence.update(previous=previous_manifest['version'], candidate=next_manifest['version'])
    env = {'HOME': str(user), 'PATH': '', 'LC_ALL': 'C'}
    f = Fixture(work, 'canonical')

    def call(tool, *words, error=None):
        run = subprocess.run([str(tool), *map(str, words)], cwd=f.root, env=env,
                             capture_output=True, text=True, timeout=120)
        value = json.loads(run.stdout or run.stderr)
        if error:
            assert run.returncode == 2 and value['error']['code'] == error, value
        else:
            assert run.returncode == 0 and value.get('ok'), value
        return value

    def ce(*words, **kwargs):
        return call(prefix / 'bin/shin-ce', *words, '--home', home, **kwargs)

    try:
        call(old / 'bin/shin-ce', 'install', '--prefix', prefix, '--bin-dir', commands, '--home', home)
        words = 'alpha beta gamma delta epsilon zeta eta theta iota'.split()
        query = ' '.join(words)
        docs = f.root / 'docs'
        docs.mkdir()
        for index in range(4):
            (docs / f'other-{index}.md').write_text(
                f'# Other {index}\n\n## First\n\n{query}.\n\n## Second\n\n{query}.\n')
        contract = {'document': 'neutral-policy', 'status': 'accepted', 'summary': query,
                    'operations': {'last': words[-1]}}
        path = docs / 'policy.json'
        path.write_text(json.dumps(contract, indent=2))
        profile = json.loads(f.profile_path.read_text())
        profile.update(bootstrapOnly=False, jsonKeySections=True, jsonKeyMaxDepth=0,
                       jsonKeyMaxSectionsPerFile=64,
                       evalCases=[{'id': 'policy-owner', 'query': query, 'mustPaths': ['docs/policy.json'],
                                   'limit': 8, 'perDocLimit': 2}],
                       packEvalCases=[{'id': 'policy-pack', 'mustPaths': ['docs/policy.json'],
                         'expectedComplete': True, 'request': {
                             'schema_version': 'shin.task-request.v1', 'request_id': 'policy-pack',
                             'task': {'objective': 'Locate the declared policy.', 'must': [{'id': 'policy', 'query': query}]},
                             'budget': {'max_evidence_bytes': 12000, 'max_records': 8,
                                        'per_query_records': 8, 'per_document_records': 2}}}])
        f.profile_path.write_text(json.dumps(profile))
        call(prefix / 'bin/shin-context', '--root', f.root, '--profile', 'observer-fixture',
             '--profile-dir', f.profiles, '--out-dir', f.out, 'build')
        call(prefix / 'bin/shin-observer', 'sample', '--config', f.config)
        assert ce('init', '--root', f.root, '--observer-config', f.config, '--manage-index')['local_ready']
        assert ce('query', query, '--root', f.root)['local_ready']
        bootstrap_root = user / 'bootstrap'
        bootstrap_root.mkdir()
        (bootstrap_root / 'limit.js').write_text('export function readLimit() { return 7; }\n')
        assert ce('init', '--root', bootstrap_root)['local_ready']
        bootstrap_workspace = home / 'projects' / hashlib.sha256(str(bootstrap_root).encode()).hexdigest()
        bootstrap_before = json.loads((bootstrap_workspace / 'state.json').read_text())
        registry = (home / 'registry/state.json').read_bytes()
        identity = (home / 'identity/state.json').read_bytes()
        workspace = home / 'projects' / hashlib.sha256(str(f.root).encode()).hexdigest()
        # Recreate the post-rollback state: an old engine cannot qualify a
        # source change that the candidate can handle, but enrollment stays.
        contract['summary'] = ' '.join(words[:-1])
        path.write_text(json.dumps(contract, indent=2))
        # Let the native updater publish under the existing grant, then inject
        # a maintenance lock at final qualification to force a real rollback.
        # No index, grant or installation journal is repaired by the fixture.
        with (workspace / 'observer.lock').open('r+') as lock:
            readme = (f.root / 'README.md').read_bytes()
            activation = subprocess.Popen([str(candidate / 'bin/shin-ce'), 'update', '--prefix', str(prefix), '--progress-json'],
                                          cwd=f.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            injected = False
            rejected = None
            timeout = threading.Timer(180, activation.kill)
            timeout.daemon = True
            timeout.start()
            try:
                for line in activation.stderr:
                    frame = json.loads(line)
                    if 'step' not in frame:
                        rejected = frame
                        continue
                    if frame['step'] == 14 and not frame['received'] and not injected:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        (f.root / 'README.md').write_text('# Fixture\n\nSource changed during activation.\n')
                        injected = True
                    if frame['step'] == 16 and injected:
                        (f.root / 'README.md').write_bytes(readme)
                        fcntl.flock(lock, fcntl.LOCK_UN)
                output = activation.stdout.read()
                if output:
                    rejected = json.loads(output)
                assert activation.wait(timeout=30) == 2 and injected, {'injected': injected, 'result': rejected}
                assert rejected['error']['code'] == 'installation-update-rolled-back', rejected
                evidence['initial_rollback'] = rejected
                if not args.expected_error:
                    diagnostic = rejected['error']['diagnostic']
                    assert hashlib.sha256(str(f.root).encode()).hexdigest() in json.dumps(diagnostic), diagnostic
                    assert str(f.root) not in json.dumps(diagnostic) and len(json.dumps(diagnostic, separators=(',', ':'))) <= 1024
            finally:
                timeout.cancel()
                if activation.poll() is None:
                    activation.kill()
                    activation.wait(timeout=15)
        restored = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert restored['version'] == previous_manifest['version'] and restored['integrity_verified']
        assert not restored['recovery_pending'], restored
        binding = (workspace / 'state.json').read_bytes()
        grant = (workspace / 'publication/state.json').read_bytes()
        deadline = time.monotonic() + 25
        while True:
            if (home / 'service/rpc.sock').exists():
                status = ce('service-status')
                if status['enrollment_errors'].get(str(f.root)) in ('project-check-failed', 'refresh-validation-failed'):
                    break
            assert time.monotonic() < deadline, status if 'status' in locals() else 'no endpoint'
            time.sleep(.05)
        evidence['previous_enrollment_error'] = status['enrollment_errors'][str(f.root)]
        assert evidence['previous_enrollment_error'] in ('project-check-failed', 'refresh-validation-failed')
        ce('status', '--root', f.root, error='service-project-denied')
        history = home / 'service/history/telemetry.sqlite'
        history_inode = history.stat().st_ino
        with sqlite3.connect(f'file:{history}?mode=ro', uri=True) as db:
            db.row_factory = sqlite3.Row
            requests_before = [dict(row) for row in db.execute("SELECT * FROM events WHERE kind='request'")]
            rollups_before = [dict(row) for row in db.execute('SELECT * FROM refresh_hourly')]
            coverage_before = db.execute('SELECT * FROM refresh_rollup_meta').fetchall()
        assert requests_before
        evidence['retained_request_rows'] = len(requests_before)
        evidence['retained_refresh_buckets'] = len(rollups_before)
        old_payload = hashlib.sha256((prefix / 'shin-kit.json').read_bytes()).hexdigest()
        result = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, error=args.expected_error)
        evidence['update_result'] = result
        if args.expected_error:
            assert hashlib.sha256((prefix / 'shin-kit.json').read_bytes()).hexdigest() == old_payload
            preserved = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
            assert preserved['version'] == previous_manifest['version'] and preserved['integrity_verified']
            assert not preserved['recovery_pending']
            evidence['old_kit_preserved'] = True
        else:
            assert result['changed'] and result['version'] == next_manifest['version']
            installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
            assert installed['integrity_verified'] and not installed['recovery_pending']
            after = ce('service-status')
            assert after['service_running'] and not after['registry_error'] and not after['enrollment_errors']
            assert ce('status', '--root', f.root)['local_ready']
            assert ce('query', query, '--root', f.root)['local_ready']
            assert ce('status', '--root', bootstrap_root)['local_ready']
            bootstrap_after = json.loads((bootstrap_workspace / 'state.json').read_text())
            for key in bootstrap_before.keys() - {'core_version', 'prepared_snapshot'}:
                assert bootstrap_after[key] == bootstrap_before[key]
            assert bootstrap_after['core_version'] == next_manifest['core_version']
            before_binding = json.loads(binding)
            after_binding = json.loads((workspace / 'state.json').read_bytes())
            # Only ordinary publication may advance the prepared generation.
            before_binding.pop('prepared_snapshot')
            after_binding.pop('prepared_snapshot')
            assert after_binding == before_binding
            assert (workspace / 'publication/state.json').read_bytes() == grant
            assert history.stat().st_ino == history_inode
            with sqlite3.connect(f'file:{history}?mode=ro', uri=True) as db:
                db.row_factory = sqlite3.Row
                assert db.execute('pragma quick_check').fetchone()[0] == 'ok'
                rows = {row['id']: dict(row) for row in db.execute("SELECT * FROM events WHERE kind='request'")}
                assert all(row.items() <= rows.get(row['id'], {}).items() for row in requests_before)
                rollups = {(row['hour_ms'], row['project']): dict(row) for row in db.execute('SELECT * FROM refresh_hourly')}
                for before in rollups_before:
                    after = rollups[(before['hour_ms'], before['project'])]
                    for key in before.keys() - {'hour_ms', 'project'}:
                        if before[key] is not None:
                            assert after[key] is not None and after[key] >= before[key], key
                assert db.execute('SELECT * FROM refresh_rollup_meta').fetchall() == coverage_before
            evidence.update(automatic_service_recovery=True, target_project_ready=True,
                            canonical_authority_preserved=True, history_preserved=True)
        assert (home / 'registry/state.json').read_bytes() == registry
        assert (home / 'identity/state.json').read_bytes() == identity
        evidence['ok'] = True
    finally:
        if (home / 'service/rpc.sock').exists():
            stopped = subprocess.run([str(prefix / 'bin/shin-ce'), 'service-stop', '--home', str(home)],
                                     env=env, capture_output=True, timeout=30)
            assert stopped.returncode == 0, (stopped.stdout, stopped.stderr)
            deadline = time.monotonic() + 15
            with (home / 'service/observer.lock').open('r+') as lock:
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.flock(lock, fcntl.LOCK_UN)
                        break
                    except BlockingIOError:
                        assert time.monotonic() < deadline
                        time.sleep(.02)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(evidence, indent=2) + '\n')

print(json.dumps(evidence))
