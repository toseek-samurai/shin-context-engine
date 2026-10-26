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
parser.add_argument('--scenario', choices=['forward', 'rollback', 'rollback-resume', 'interrupted'], default='forward')
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
        if args.scenario == 'forward':
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
        expected = old_manifest if args.scenario in ['rollback','rollback-resume'] else new_manifest
        installed = call(prefix / 'bin/shin-ce', 'installation', '--prefix', prefix)
        assert installed['version'] == expected['version'] and installed['integrity_verified'] and not installed['recovery_pending'], installed
        for root in roots:
            result = ce('query', 'readLimit', '--service', '--root', root)
            assert result['local_ready'] and 'readLimit' in result['source_context'] and result['transport'] == 'local-socket-v1', result
        after_generators = generators()
        assert after_generators == ([before_generators[0]] * 2 if args.scenario in ['rollback','rollback-resume'] else [new_identity['generator_version']] * 2), after_generators
        for workspace, before in zip(workspaces, bindings):
            after = json.loads((workspace / 'state.json').read_text())
            assert {k:v for k,v in before.items() if k not in {'core_version','generator_version','prepared_snapshot'}} == {
                k:v for k,v in after.items() if k not in {'core_version','generator_version','prepared_snapshot'}}
        after_grant = json.loads(grant_path.read_text()); after_grant.pop('last_snapshot')
        assert after_grant == grant
        assert all(p.read_bytes() == content for p, content in protected.items())
        assert history.stat().st_ino == inode
        with closing(sqlite3.connect(f'file:{history}?mode=ro', uri=True)) as db:
            current = db.execute('SELECT kind,at_ms,project,method,outcome,request_id FROM events').fetchall()
            assert all(row in current for row in retained)
            assert db.execute('PRAGMA quick_check').fetchone()[0] == 'ok'
        evidence.update(ok=True, generators=after_generators, retained_events=len(retained),
                        bindings_preserved=True, authority_preserved=True, history_preserved=True)
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
