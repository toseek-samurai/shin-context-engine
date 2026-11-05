#!/usr/bin/env python3
"""Real previous-kit upgrade with stale source and hard authority controls.

Only disposable installs are modified; archive transport is tested separately.
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
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from integration_test import Fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--previous-kit', type=Path, required=True)
parser.add_argument('--candidate-kit', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()
evidence = {'live_installation_changed': False}
with tempfile.TemporaryDirectory(prefix='shin-same-core-', dir='/tmp') as directory:
    work = Path(directory).resolve()
    old, candidate = work / 'previous', args.candidate_kit.resolve()
    shutil.copytree(args.previous_kit.resolve(), old, ignore=shutil.ignore_patterns('.shin-installation.json'))
    previous = json.loads((old / 'shin-kit.json').read_text())
    target = json.loads((candidate / 'shin-kit.json').read_text())
    assert previous['core_version'] == target['core_version'] and previous['version'] != target['version']
    user = work / 'user'; user.mkdir(mode=0o700)
    prefix, home, commands = user / 'kit', user / 'data', user / 'bin'
    env = {**os.environ, 'HOME': str(user), 'PATH': '', 'LC_ALL': 'C'}
    roots = [user / name for name in ('changing', 'healthy')]
    for root in roots:
        root.mkdir(); (root / 'quota.cpp').write_text('int quota() { return 1; }\n')

    def call(executable, *words, error=None):
        run = subprocess.run([str(executable), *map(str, words)], cwd=user, env=env,
                             capture_output=True, text=True, timeout=180)
        result = json.loads(run.stdout or run.stderr)
        if error:
            assert run.returncode and result['error']['code'] == error, result
        else:
            assert run.returncode == 0 and result.get('ok'), (run.returncode, result)
        return result

    def stop():
        if (home / 'service/rpc.sock').exists():
            call(prefix / 'bin/shin-ce', 'service-stop', '--home', home)
            end = time.monotonic() + 15
            while (home / 'service/rpc.sock').exists():
                assert time.monotonic() < end
                time.sleep(.02)
    try:
        call(old / 'bin/shin-ce', 'install', '--prefix', prefix, '--bin-dir', commands, '--home', home)
        for root in roots:
            assert call(prefix / 'bin/shin-ce', 'init', '--root', root, '--home', home)['local_ready']
        stop()
        receipt = (prefix / '.shin-installation.json').read_bytes()
        registry = (home / 'registry/state.json').read_bytes()
        binding = home / 'projects' / hashlib.sha256(str(roots[0]).encode()).hexdigest() / 'state.json'
        original_binding = binding.read_bytes()
        changed = json.loads(original_binding); changed['profile_config_hash'] = 'fnv1a64:0000000000000000'
        binding.write_text(json.dumps(changed)); binding.chmod(0o600)
        call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, error='profile-policy-revision-conflict')
        assert (prefix / '.shin-installation.json').read_bytes() == receipt
        binding.write_bytes(original_binding)
        (roots[0] / 'quota.cpp').write_text('int quota() { return 2; }\n')
        result = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix)
        assert result['changed'] and result['version'] == target['version'], result
        assert result['projects_pending'] == 1 and not result['all_projects_ready'], result
        assert (home / 'registry/state.json').read_bytes() == registry
        assert binding.read_bytes() == original_binding
        installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert installed['integrity_verified'] and not installed['recovery_pending']
        assert installed['project_preparation']['pending_projects'] == 1
        call(prefix / 'bin/shin-ce', 'service-start', '--home', home)
        for root in roots:
            assert call(prefix / 'bin/shin-ce', 'status', '--home', home, '--root', root)['local_ready']
        stop()
        # A strict canonical eval failure already present under the old engine
        # must not block replacing the runtime. Its context remains withheld.
        prefix, home, commands = user / 'kit-quality', user / 'data-quality', user / 'bin-quality'
        call(old / 'bin/shin-ce', 'install', '--prefix', prefix, '--bin-dir', commands, '--home', home)
        fixture = Fixture(work, 'quality')
        fixture.write_source('# Ceiling\n\nBounded retry ceiling.\n')
        profile = json.loads(fixture.profile_path.read_text())
        profile.update(bootstrapOnly=False, evalCases=[{'id':'bounded-search', 'query':'retry ceiling',
            'mustPaths':['README.md'], 'maxMatchedSections':32}], packEvalCases=[{'id':'ceiling-pack', 'mustPaths':['README.md'], 'expectedComplete':True,
            'request':{'schema_version':'shin.task-request.v1','request_id':'ceiling-pack',
                'task':{'objective':'Locate retry ceiling.','must':[{'id':'rule','query':'retry ceiling'}]},
                'budget':{'max_evidence_bytes':8192,'max_records':4,'per_query_records':4,'per_document_records':2}}}])
        fixture.profile_path.write_text(json.dumps(profile))
        policy = fixture.profile_path.read_bytes()
        call(prefix / 'bin/shin-context', '--root', fixture.root, '--profile', 'observer-fixture',
             '--profile-dir', fixture.profiles, '--out-dir', fixture.out, 'build')
        call(prefix / 'bin/shin-observer', 'sample', '--config', fixture.config)
        assert call(prefix / 'bin/shin-ce', 'init', '--root', fixture.root, '--home', home,
                    '--observer-config', fixture.config, '--manage-index')['local_ready']
        fixture.write_source('# Ceiling\n\nBounded retry ceiling.\n' + ''.join(f'\n## Ceiling {n}\n\nAnother retry ceiling.\n' for n in range(100)))
        watch = home / 'projects' / hashlib.sha256(str(fixture.root).encode()).hexdigest() / 'watch/state.json'
        end = time.monotonic() + 45
        while True:
            observed = json.loads(watch.read_text()) if watch.exists() else {}
            if observed.get('phase') == 'blocked':
                assert 'retrieval-ceiling-violated' in json.dumps(observed), observed
                break
            assert time.monotonic() < end, observed
            time.sleep(.1)
        pending = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix)
        assert pending['changed'] and pending['projects_pending'] == 1, pending
        assert call(prefix / 'bin/shin-ce', 'service-status', '--home', home)['service_running']
        assert fixture.profile_path.read_bytes() == policy
        refused = subprocess.run([str(prefix / 'bin/shin-ce'), 'query', 'retry ceiling',
            '--root', str(fixture.root), '--home', str(home)], env=env, capture_output=True, text=True, timeout=45)
        assert refused.returncode != 0, refused.stdout
        assert 'source_context' not in (refused.stdout + refused.stderr)
        evidence['preexisting_strict_eval_did_not_block_update'] = True
        evidence['unqualified_context_remains_withheld'] = True
        # Capture a ready baseline, then change project content while a native
        # activation is journaled and force a real damaged-payload rollback.
        fixture.write_source('# Ceiling\n\nBounded retry ceiling.\n')
        end = time.monotonic() + 45
        while True:
            probe = subprocess.run([str(prefix / 'bin/shin-ce'), 'status', '--root', str(fixture.root), '--home', str(home)],
                cwd=user, env=env, capture_output=True, text=True, timeout=35)
            if probe.returncode == 0 and json.loads(probe.stdout).get('local_ready'): break
            assert time.monotonic() < end, (probe.stdout, probe.stderr)
            time.sleep(.1)
        stop()
        control = Path(json.loads((prefix / '.shin-installation.json').read_text())['control'])
        state_path = control / 'state.json'
        state = json.loads(state_path.read_text())
        retained = state.pop('previous')
        state['phase'] = 'activating'
        state['update'] = {'receipt':state['receipt'], 'kit':state['kit'],
            'old_receipt':retained['receipt'], 'old_kit':retained['kit'],
            'runtime':{'managed':False, 'running':True, 'unready_roots':[]},
            'registry_sha256':hashlib.sha256((home / 'registry/state.json').read_bytes()).hexdigest(),
            'step':'swapping'}
        state_path.write_text(json.dumps(state)); state_path.chmod(0o600)
        fixture.write_source('# Ceiling\n\nBounded retry ceiling.\n' + ''.join(f'\n## Ceiling {n}\n\nAnother retry ceiling.\n' for n in range(100)))
        (prefix / 'bin/shin-context').chmod(0o600)
        call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, error='installation-update-rolled-back')
        restored = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert restored['version'] == previous['version'] and restored['integrity_verified'] and not restored['recovery_pending']
        assert restored['last_rollback']['service_restored'] and restored['last_rollback']['pending_projects'] == 1
        assert fixture.profile_path.read_bytes() == policy
        evidence['new_eval_failure_does_not_block_binary_rollback'] = True
        evidence.update(ok=True, previous=previous['version'], candidate=target['version'],
                        stale_source_did_not_block_update=True, authority_change_refused=True,
                        stopped_service_preserved=True, pending_projects=1, background_refresh_restored=True)
    finally:
        stop()
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(evidence, indent=2) + '\n')
print(json.dumps(evidence))
