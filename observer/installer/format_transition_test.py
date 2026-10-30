#!/usr/bin/env python3
"""Qualify a real previous/candidate kit pair in disposable homes, without fetching or installing live."""
import argparse
from contextlib import closing
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import shutil
import stat
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from integration_test import Fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--previous-kit', required=True, type=Path)
parser.add_argument('--candidate-kit', required=True, type=Path)
parser.add_argument('--report', required=True, type=Path)
parser.add_argument('--scenario', choices=['forward', 'rollback', 'rollback-resume', 'interrupted', 'profile-policy-rejected', 'marker-edit-rejected'], default='forward')
parser.add_argument('--with-marker', action='store_true')
parser.add_argument('--fresh-updater', action='store_true', help='exercise the independent updater entry for the forward transition')
args = parser.parse_args()
if args.fresh_updater and args.scenario != 'forward':
    parser.error('--fresh-updater requires the forward scenario')
old, candidate = args.previous_kit.resolve(), args.candidate_kit.resolve()
old_manifest = json.loads((old / 'shin-kit.json').read_text())
new_manifest = json.loads((candidate / 'shin-kit.json').read_text())
old_identity = json.loads(subprocess.check_output([str(old / 'bin/shin-context'), '--version'], text=True))
new_identity = json.loads(subprocess.check_output([str(candidate / 'bin/shin-context'), '--version'], text=True))
assert old_identity['version'] == old_manifest['core_version'] and new_identity['version'] == new_manifest['core_version']
evidence = {'previous': old_manifest['version'], 'candidate': new_manifest['version'],
            'scenario': args.scenario, 'live_installation_changed': False}

with tempfile.TemporaryDirectory(prefix='shin-format-update-', dir=Path.home() if args.fresh_updater else '/tmp') as temporary:
    work = Path(temporary).resolve()
    user = work / 'user'; user.mkdir(mode=0o700)
    prefix, home, commands = user / 'kit', user / 'data', user / 'bin'
    env = {'HOME': str(user), 'PATH': '', 'LC_ALL': 'C'}
    f = Fixture(work, 'canonical')
    profile = json.loads(f.profile_path.read_text())
    profile.update(sourceFactRoots=['src'], sourceFactExtensions=['.cpp'], bootstrapOnly=False,
        evalCases=[{'id':'limit-owner','query':'readLimit','mustPaths':['src/limit.cpp'],'limit':8,'perDocLimit':2}],
        packEvalCases=[{'id':'limit-pack','mustPaths':['src/limit.cpp'],'expectedComplete':True,'request':{
            'schema_version':'shin.task-request.v1','request_id':'limit-pack',
            'task':{'objective':'Locate the declared limit.','must':[{'id':'limit','query':'readLimit'}]},
            'budget':{'max_evidence_bytes':12000,'max_records':8,'per_query_records':8,'per_document_records':2}}}])
    marker = None
    if args.with_marker:
        marker = f.root / 'docs/machine/shin-project-adoption-v1.json'
        marker.parent.mkdir(parents=True)
        template = Path(__file__).resolve().parents[2] / 'tests/fixtures/adoption/root/docs/machine/shin-project-adoption-v1.json'
        adoption = json.loads(template.read_text())
        adoption['engine']['generatorIdentity'] = old_identity['generator_version']
        adoption['profile']['id'] = 'observer-fixture'
        adoption['index']['key'] = 'index'
        marker.write_text(json.dumps(adoption, indent=2) + '\n')
        (f.root / 'AGENTS.md').write_text('Context epoch: fixture-shin-v1.12-2026-07-19.\n'
            'Use docs/machine/shin-project-adoption-v1.json and profile observer-fixture.\n')
        profile['agentStartPaths'] += ['AGENTS.md', 'docs/machine/shin-project-adoption-v1.json']
    f.profile_path.write_text(json.dumps(profile))
    (f.root / 'src').mkdir(); (f.root / 'src/limit.cpp').write_text('int readLimit() { return 7; }\n')
    bootstrap = user / 'project'; bootstrap.mkdir()
    (bootstrap / 'limit.cpp').write_text('int readLimit() { return 17; }\n')
    roots = [f.root, bootstrap]
    workspaces = [home / 'projects' / hashlib.sha256(str(root).encode()).hexdigest() for root in roots]
    outputs = [f.out, workspaces[1] / 'index']

    def call(tool, *words, error=None):
        result = subprocess.run([str(tool), *map(str, words)], cwd=work, env=env,
                                capture_output=True, text=True, timeout=150)
        value = json.loads(result.stdout or result.stderr)
        if error:
            assert result.returncode == 2 and value['error']['code'] == error, value
        else:
            assert result.returncode == 0 and value.get('ok'), value
        return value

    def ce(*words):
        return call(prefix / 'bin/shin-ce', *words, '--home', home)

    def generators():
        return [json.loads((out / 'docs/kb/generated/manifest.json').read_text())['generator_version'] for out in outputs]

    child = None
    try:
        call(old / 'bin/shin-ce', 'install', '--prefix', prefix, '--bin-dir', commands, '--home', home)
        call(prefix / 'bin/shin-context', '--root', f.root, '--profile', 'observer-fixture',
             '--profile-dir', f.profiles, '--out-dir', f.out, 'build')
        call(prefix / 'bin/shin-observer', 'sample', '--config', f.config)
        ce('init', '--root', f.root, '--observer-config', f.config, '--manage-index')
        ce('init', '--root', bootstrap)
        ce('service-start')
        for root in roots:
            assert ce('query', 'readLimit', '--service', '--root', root)['local_ready']
        before_generators = generators()
        assert before_generators == [old_identity['generator_version']] * 2, before_generators
        bindings = [json.loads((workspace / 'state.json').read_text()) for workspace in workspaces]
        protected = {p: p.read_bytes() for p in [home / 'registry/state.json', home / 'identity/state.json', f.config, f.profile_path,
                                                bootstrap / 'limit.cpp', f.root / 'src/limit.cpp', f.root / 'README.md']}
        if marker:
            protected[marker] = marker.read_bytes()
            marker_permissions = stat.S_IMODE(marker.stat().st_mode)
            protected[f.root / 'AGENTS.md'] = (f.root / 'AGENTS.md').read_bytes()
        grant_path = workspaces[0] / 'publication/state.json'
        grant = json.loads(grant_path.read_text()); grant.pop('last_snapshot')
        history = home / 'service/history/telemetry.sqlite'; inode = history.stat().st_ino
        history_deadline = time.monotonic() + 5
        while True:
            with closing(sqlite3.connect(f'file:{history}?mode=ro', uri=True)) as db:
                retained = db.execute('SELECT kind,at_ms,project,method,outcome,request_id FROM events').fetchall()
            if sum(row[0] == 'request' and row[3] == 'query' for row in retained) >= 2: break
            assert time.monotonic() < history_deadline, 'both service queries must reach retained history'
            time.sleep(.02)
        if args.scenario == 'marker-edit-rejected':
            assert marker
            edited = json.loads(marker.read_text()); edited['summary'] += ' Owner edit awaiting review.'
            marker.write_text(json.dumps(edited, indent=2) + '\n')
            protected[marker] = marker.read_bytes()
            evidence['update'] = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix,
                                      error='installation-marker-transition-conflict')
        elif args.scenario == 'profile-policy-rejected':
            altered = work / 'unqualified-candidate'
            shutil.copytree(candidate, altered)
            candidate = altered
            profile_path = candidate / 'share/shin-context/profiles/generic-source.json'
            unqualified = json.loads(profile_path.read_text())
            unqualified['sourceFactMaxFiles'] += 1
            profile_path.write_text(json.dumps(unqualified) + '\n')
            inventory_path = candidate / 'shin-kit.json'
            inventory = json.loads(inventory_path.read_text())
            entry = next(row for row in inventory['files'] if row['path'] == 'share/shin-context/profiles/generic-source.json')
            entry.update(size=profile_path.stat().st_size, sha256=hashlib.sha256(profile_path.read_bytes()).hexdigest())
            inventory_path.write_text(json.dumps(inventory) + '\n')
            evidence['update'] = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix,
                                      error='installation-profile-transition-unsupported')
        elif args.scenario == 'forward':
            evidence['update'] = call(candidate / 'bin/shin-updater', 'install-local', '--kit', candidate, '--prefix', prefix) if args.fresh_updater else \
                call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix)
            evidence['fresh_updater'] = args.fresh_updater
        else:
            # Progress is emitted after candidate publication and before final
            # qualification. A real project lock triggers rollback; killing at
            # that boundary exercises journal continuation without state edits.
            with (workspaces[1] / 'observer.lock').open('r+') as lock:
                child = subprocess.Popen([str(candidate / 'bin/shin-ce'), 'update', '--prefix', str(prefix), '--progress-json'],
                    cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                pending = b''; injected = False; final_response = None; until = time.monotonic() + 150
                while child.poll() is None:
                    assert time.monotonic() < until, 'update progress timeout'
                    if not select.select([child.stderr], [], [], .1)[0]: continue
                    block = os.read(child.stderr.fileno(), 8192)
                    if not block: break
                    pending += block
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1); frame = json.loads(line)
                        if 'error' in frame: final_response = frame
                        if frame.get('step') == 14 and not frame.get('received') and not injected:
                            os.kill(child.pid, signal.SIGSTOP)
                            assert generators() == [new_identity['generator_version']] * 2
                            injected = True
                            if args.scenario == 'interrupted': child.kill(); break
                            lock_deadline = time.monotonic() + 5
                            while True:
                                try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB); break
                                except BlockingIOError:
                                    assert time.monotonic() < lock_deadline
                                    time.sleep(.005)
                            os.kill(child.pid, signal.SIGCONT)
                        if frame.get('step') == 16 and injected and args.scenario != 'rollback-resume':
                            fcntl.flock(lock, fcntl.LOCK_UN)
                output, error = child.communicate(timeout=20)
                assert injected, (output, error, pending)
                if args.scenario in ['rollback', 'rollback-resume']:
                    for line in (output + error + pending).splitlines():
                        frame = json.loads(line)
                        if 'error' in frame: final_response = frame
                    value = final_response
                    evidence['update'] = value
                    expected_error = 'installation-update-recovery-required' if args.scenario == 'rollback-resume' else 'installation-update-rolled-back'
                    assert child.returncode == 2 and value['error']['code'] == expected_error, value
                    if args.scenario == 'rollback-resume':
                        fcntl.flock(lock, fcntl.LOCK_UN)
                        evidence['resumed_rollback'] = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix, error='installation-update-rolled-back')
                else:
                    evidence['update'] = call(candidate / 'bin/shin-ce', 'update', '--prefix', prefix)
                child = None
        old_preserved = args.scenario in ['rollback','rollback-resume','profile-policy-rejected','marker-edit-rejected']
        expected = old_manifest if old_preserved else new_manifest
        installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert installed['version'] == expected['version'] and installed['integrity_verified'] and not installed['recovery_pending'], installed
        for root in roots:
            if args.scenario == 'marker-edit-rejected' and root == f.root: continue
            result = ce('query', 'readLimit', '--service', '--root', root)
            assert result['local_ready'] and 'readLimit' in result['source_context'] and result['transport'] == 'local-socket-v1', result
        after_generators = generators()
        assert after_generators == ([before_generators[0]] * 2 if old_preserved else [new_identity['generator_version']] * 2), after_generators
        profile_revisions = []
        for workspace, before in zip(workspaces, bindings):
            after = json.loads((workspace / 'state.json').read_text())
            rebuilt_fields = {'core_version','generator_version','prepared_snapshot'}
            if before.get('profile_config_hash') != after.get('profile_config_hash'):
                assert expected == new_manifest and before['schema_version'] == 'shin.local-project-binding.v1'
                assert before['profile_dir'] == str(prefix / 'share/shin-context/profiles')
                previous_profile = old / 'share/shin-context/profiles/generic-source.json'
                target_profile = candidate / 'share/shin-context/profiles/generic-source.json'
                old_policy, new_policy = json.loads(previous_profile.read_text()), json.loads(target_profile.read_text())
                old_extensions, new_extensions = set(old_policy.pop('sourceFactExtensions')), set(new_policy.pop('sourceFactExtensions'))
                assert old_policy == new_policy and old_extensions <= new_extensions
                added = sorted(new_extensions - old_extensions)
                assert set(added) <= {'.m', '.mm'}
                revision = 1469598103934665603
                for byte in target_profile.read_bytes():
                    revision = ((revision ^ byte) * 1099511628211) & ((1 << 64) - 1)
                assert after['profile_config_hash'] == f'fnv1a64:{revision:016x}'
                profile_revisions.append({'from': before['profile_config_hash'], 'to': after['profile_config_hash'], 'added_extensions': added})
                rebuilt_fields.add('profile_config_hash')
            assert {k:v for k,v in before.items() if k not in rebuilt_fields} == {
                k:v for k,v in after.items() if k not in rebuilt_fields}
        if marker:
            expected_marker = protected[marker]
            if not old_preserved:
                expected_marker = expected_marker.replace(old_identity['generator_version'].encode(), new_identity['generator_version'].encode())
            if args.scenario not in ('profile-policy-rejected', 'marker-edit-rejected'):
                # Native approval records an explicit marker hash (v3), including
                # when rollback restores the implicit original v2 approval.
                grant['approved_marker_hash'] = hashlib.sha256(expected_marker).hexdigest()
                grant['schema_version'] = 'shin.local-publication.v3'
            assert marker.read_bytes() == expected_marker
            assert stat.S_IMODE(marker.stat().st_mode) == marker_permissions
            protected[marker] = expected_marker
            evidence['marker_transition'] = {'only_generator_pin_changed': True, 'restored': old_preserved,
                'approved_hash': hashlib.sha256(expected_marker).hexdigest() if args.scenario != 'marker-edit-rejected' else
                    grant.get('approved_marker_hash', grant['route']['marker_hash'])}
        after_grant = json.loads(grant_path.read_text()); after_grant.pop('last_snapshot')
        assert after_grant == grant, (after_grant, grant)
        assert all(p.read_bytes() == content for p, content in protected.items())
        assert history.stat().st_ino == inode
        with closing(sqlite3.connect(f'file:{history}?mode=ro', uri=True)) as db:
            current = db.execute('SELECT kind,at_ms,project,method,outcome,request_id FROM events').fetchall()
            assert all(row in current for row in retained)
            assert db.execute('PRAGMA quick_check').fetchone()[0] == 'ok'
        evidence.update(ok=True, generators=after_generators, retained_events=len(retained),
                        bindings_preserved=True, profile_revisions=profile_revisions, authority_preserved=True, history_preserved=True)
    finally:
        if child and child.poll() is None: child.kill(); child.communicate(timeout=20)
        if (home / 'service/rpc.sock').exists():
            for executable in [prefix / 'bin/shin-ce', candidate / 'bin/shin-ce', old / 'bin/shin-ce']:
                result = subprocess.run([str(executable), 'service-stop', '--home', str(home)],
                                        env=env, capture_output=True, timeout=30)
                if result.returncode == 0: break
            assert result.returncode == 0, (result.stdout, result.stderr)
        # Stop acknowledgement precedes the final watch/history flush. Do not
        # remove the fixture while its service still owns the lifetime lease.
        lease = home / 'service/observer.lock'
        if lease.exists():
            with lease.open('r+') as lock:
                deadline = time.monotonic() + 20
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.flock(lock, fcntl.LOCK_UN)
                        break
                    except BlockingIOError:
                        assert time.monotonic() < deadline, 'fixture service did not stop'
                        time.sleep(.02)
        args.report.write_text(json.dumps(evidence, indent=2) + '\n')
evidence['cleanup_complete'] = True
args.report.write_text(json.dumps(evidence, indent=2) + '\n')
print(json.dumps({k:v for k,v in evidence.items() if k != 'update'}))
