#!/usr/bin/env python3
"""Opt-in update proof with a real previous kit and a bounded canonical route.

Only disposable installs and data are modified. No releases are downloaded.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
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
args = parser.parse_args()
evidence = {'live_installation_changed': False, 'network_delivery_tested': False}
with tempfile.TemporaryDirectory(prefix='shin-source-limit-update-', dir='/tmp') as directory:
    work = Path(directory).resolve()
    old, candidate = work / 'previous', work / 'candidate'
    for source, destination in ((args.previous_kit, old), (args.candidate_kit, candidate)):
        shutil.copytree(source.resolve(), destination, ignore=shutil.ignore_patterns('.shin-installation.json'))
    previous = json.loads((old / 'shin-kit.json').read_text())
    target = json.loads((candidate / 'shin-kit.json').read_text())
    assert previous['core_version'] == target['core_version'], 'This case covers an Observer-only update'
    assert previous['version'] != target['version']
    user = work / 'user'
    user.mkdir(mode=0o700)
    prefix, home, commands = user / 'kit', user / 'data', user / 'bin'
    env = {'HOME': str(user), 'PATH': '', 'LC_ALL': 'C'}
    f = Fixture(work, 'canonical')
    f.write_source('# Source contract\n\nThe retry ceiling belongs to readLimit.\n')
    (f.root / 'src').mkdir()
    (f.root / 'src/limits.cpp').write_text('int readLimit() { return 7; }\n')
    profile = json.loads(f.profile_path.read_text())
    profile.update(bootstrapOnly=False, sourceFactRoots=['src'], sourceFactExtensions=['.cpp'], sourceFactMaxFiles=1,
                   evalCases=[{'id': 'ceiling', 'query': 'retry ceiling', 'mustPaths': ['README.md']}],
                   packEvalCases=[{'id': 'ceiling-pack', 'mustPaths': ['README.md'], 'expectedComplete': True,
                       'request': {'schema_version': 'shin.task-request.v1', 'request_id': 'ceiling-pack',
                           'task': {'objective': 'Locate the ceiling.', 'must': [{'id': 'rule', 'query': 'retry ceiling'}]},
                           'budget': {'max_evidence_bytes': 8192, 'max_records': 4,
                                      'per_query_records': 4, 'per_document_records': 2}}}])
    f.profile_path.write_text(json.dumps(profile))

    def call(tool, *words, error=None):
        run = subprocess.run([str(tool), *map(str, words)], cwd=f.root, env=env,
                             capture_output=True, text=True, timeout=120)
        value = json.loads(run.stdout or run.stderr)
        if error:
            assert run.returncode == 2 and value['error']['code'] == error, value
        else:
            assert run.returncode == 0 and value.get('ok'), value
        return value

    def ce(*words, **options):
        return call(prefix / 'bin/shin-ce', *words, '--home', home, **options)

    def save(path, value):
        path.write_text(json.dumps(value)); path.chmod(0o600)

    def restart_previous_service():
        ce('service-stop')
        deadline = time.monotonic() + 15
        while (home / 'service/rpc.sock').exists():
            assert time.monotonic() < deadline
            time.sleep(.02)
        ce('service-start')

    try:
        call(old / 'bin/shin-ce', 'install', '--prefix', prefix, '--bin-dir', commands, '--home', home)
        call(prefix / 'bin/shin-context', '--root', f.root, '--profile', 'observer-fixture',
             '--profile-dir', f.profiles, '--out-dir', f.out, 'build')
        call(prefix / 'bin/shin-observer', 'sample', '--config', f.config)
        assert ce('init', '--root', f.root, '--observer-config', f.config, '--manage-index')['local_ready']
        workspace = home / 'projects' / hashlib.sha256(str(f.root).encode()).hexdigest()
        binding_path = workspace / 'state.json'
        binding = binding_path.read_bytes()
        grant = (workspace / 'publication/state.json').read_bytes()
        manifest = (f.out / 'docs/kb/generated/manifest.json').read_bytes()
        policy = f.profile_path.read_bytes()
        registry = (home / 'registry/state.json').read_bytes()
        identity = (home / 'identity/state.json').read_bytes()
        (f.root / 'src/zz-growth.cpp').write_text('int growth() { return 8; }\n')
        # Restart the actual old engine into the same refusal seen after rollback.
        restart_previous_service()
        status = ce('service-status')
        assert status['enrollment_errors'].get(str(f.root)) == 'incomplete-source-observation', status
        evidence['previous_refusal'] = 'incomplete-source-observation'
        history = home / 'service/history/telemetry.sqlite'
        history_inode = history.stat().st_ino

        # Refuse activation after admission, then restore the test's binding
        # before the verified old runtime resumes. Its source cap still applies.
        update = subprocess.Popen([str(candidate / 'bin/shin-ce'), 'update', '--prefix', str(prefix), '--progress-json'],
                                  cwd=f.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        injected = False
        rejected = None
        timer = threading.Timer(180, update.kill); timer.daemon = True; timer.start()
        try:
            for line in update.stderr:
                frame = json.loads(line)
                if 'step' not in frame:
                    rejected = frame; continue
                if frame['step'] == 13 and not injected:
                    changed = json.loads(binding); changed['core_version'] = 'unsupported-core'
                    save(binding_path, changed); injected = True
                if frame['step'] == 16 and injected:
                    binding_path.write_bytes(binding)
            output = update.stdout.read()
            if output: rejected = json.loads(output)
            assert update.wait(timeout=30) == 2 and injected, rejected
        finally:
            timer.cancel(); binding_path.write_bytes(binding)
            if update.poll() is None: update.kill(); update.wait(timeout=15)
        assert rejected['error']['code'] == 'installation-update-rolled-back', rejected
        installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert installed['version'] == previous['version'] and not installed['recovery_pending']
        assert installed['last_rollback']['service_restored'] and not installed['last_rollback']['all_projects_ready']
        assert installed['last_rollback']['pending_projects'] == 1
        evidence['baseline_rollback_confirmed'] = True

        # Legacy journals have no pre-update readiness baseline. Default recovery
        # must retain review; explicit review cannot be reused for a normal update.
        control = Path(json.loads((prefix / '.shin-installation.json').read_text())['control'])
        state_path = control / 'state.json'
        legacy = json.loads(state_path.read_text())
        legacy['resume_runtime'] = {'managed': False, 'running': True}
        save(state_path, legacy)
        call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, error='installation-service-not-ready')
        assert call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)['recovery_pending']
        try:
            changed = json.loads(binding); changed['generator_version'] = 'unsupported-generator'
            save(binding_path, changed)
            restart_previous_service()
            refused_identity = ce('service-status')
            assert refused_identity['enrollment_errors'].get(str(f.root)) == 'refresh-engine-changed', refused_identity
            call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, '--accept-degraded-rollback',
                 error='installation-service-not-ready')
            assert call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)['recovery_pending']
        finally:
            binding_path.write_bytes(binding)
            restart_previous_service()
        evidence['legacy_review_preserves_identity_refusal'] = True
        call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, '--accept-degraded-rollback',
             error='installation-update-rolled-back')
        installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert not installed['recovery_pending'] and installed['version'] == previous['version']
        assert json.loads(state_path.read_text())['last_rollback']['degraded_baseline_reviewed']
        assert not installed['last_rollback']['all_projects_ready'] and installed['last_rollback']['pending_projects'] == 1
        call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, '--accept-degraded-rollback',
             error='installation-recovery-review-not-applicable')
        evidence['legacy_recovery_requires_review'] = True

        result = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix)
        assert result['changed'] and result['version'] == target['version'], result
        assert call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)['integrity_verified']
        context = ce('query', 'readLimit', '--root', f.root)
        assert context['retrieval_ready'] and not context['canonical_ready']
        assert context['source_coverage']['reason'] == 'source-observation-limited'
        selected = ce('read', 'src/limits.cpp', '--snapshot', context['snapshot_id'], '--root', f.root)
        assert selected['native_read_complete'] and 'return 7;' in selected['source_context'], selected
        assert not ce('read', 'src/zz-growth.cpp', '--snapshot', context['snapshot_id'], '--root', f.root)['native_read_complete']
        assert binding_path.read_bytes() == binding
        assert (workspace / 'publication/state.json').read_bytes() == grant
        assert (f.out / 'docs/kb/generated/manifest.json').read_bytes() == manifest
        assert f.profile_path.read_bytes() == policy
        assert (home / 'registry/state.json').read_bytes() == registry
        assert (home / 'identity/state.json').read_bytes() == identity
        assert history.stat().st_ino == history_inode
        evidence.update(ok=True, previous=previous['version'], candidate=target['version'], partial_context_available=True,
                        data_preserved=True, policy_and_snapshot_preserved=True)
    finally:
        if (home / 'service/rpc.sock').exists():
            ce('service-stop')
            deadline = time.monotonic() + 15
            while (home / 'service/rpc.sock').exists():
                assert time.monotonic() < deadline
                time.sleep(.02)
args.report.write_text(json.dumps(evidence, indent=2) + '\n')
print(json.dumps(evidence))
