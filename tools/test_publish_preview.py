#!/usr/bin/env python3
"""Offline publication gates and uncertain-outcome readback; no GitHub mutations."""
import copy
import hashlib
import io
import json
import re
from pathlib import Path
import subprocess
import tempfile
import tarfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

import publish_preview as publisher

SHA = 'b' * 40
REPO = 'fixture/shin'
VERSION = '2.0.5-dev.21'
TAG = 'v' + VERSION


def fixtures(platform='macos', attempt=1):
    run = dict(id=123, head_sha=SHA, head_branch=TAG, run_attempt=attempt, event='push', path='.github/workflows/ci.yml',
               repository={'full_name': REPO}, head_repository={'full_name': REPO},
               status='queued', conclusion=None)
    jobs = [dict(name=name, status='completed', conclusion='success', steps=[])
            for name in sorted(publisher.MATRIX)]
    for spec in publisher.PLATFORMS.values():
        next(j for j in jobs if j['name'] == spec['package_job'])['steps'] = [
            dict(name=name, conclusion='success') for name in spec['steps']]
    jobs += [dict(name=f'publish-preview ({target})',
                  status='in_progress' if target == platform else 'queued', conclusion=None)
             for target in publisher.PLATFORMS]
    artifacts = [dict(name=name, expired=False, workflow_run={'id': 123, 'head_sha': SHA})
                 for name in sorted(publisher.artifact_names(123, attempt, platform))]
    return run, jobs, artifacts


def kit(directory):
    directory.mkdir(parents=True)
    name = f'shin-local-{VERSION}-macos-arm64.zip'
    data = b'independent synthetic kit payload'
    updater_payload = b'independent synthetic updater'
    updater_manifest = dict(schema_version='shin.updater.v1', protocol=1, version=VERSION,
        files=[dict(path='bin/shin-updater', size=len(updater_payload), executable=True,
                    sha256=hashlib.sha256(updater_payload).hexdigest())])
    payloads = {'bin/fixture': data, 'bin/shin-updater': updater_payload,
                'shin-updater.json': json.dumps(updater_manifest).encode()}
    manifest = dict(schema_version='shin.local-kit.v1', version=VERSION, core_version='2.0.5-dev.3',
                    files=[dict(path=path, size=len(value), executable=path.startswith('bin/'),
                                sha256=hashlib.sha256(value).hexdigest()) for path, value in payloads.items()])
    with zipfile.ZipFile(directory / name, 'w') as archive:
        archive.writestr(f'shin-local-{VERSION}-macos-arm64/shin-kit.json', json.dumps(manifest))
        for path, value in payloads.items():
            archive.writestr(f'shin-local-{VERSION}-macos-arm64/' + path, value)
    from package_updater import package
    package(directory / name, directory)
    digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    (directory / (name + '.sha256')).write_text(f'{digest}  {name}\n')
    (directory / 'install.sh').write_text(f"version='{VERSION}'\n")


def windows_kit(directory, attempt=1):
    directory.mkdir(parents=True)
    stem = f'shin-desktop-{VERSION}-windows-AMD64'
    paths = ['bin/' + n for n in ('shin-ce.exe', 'shin-context.exe', 'shin-observer.exe',
                                 'shin-mcp.exe', 'shin-tray.exe', 'vcruntime140.dll', 'msvcp140.dll')]
    payload = b'independent synthetic Windows payload'
    manifest = dict(schema_version='shin.local-kit.v1', version=VERSION, core_version='2.0.5-dev.3',
                    files=[dict(path=p, size=len(payload), sha256=hashlib.sha256(payload).hexdigest()) for p in paths])
    with zipfile.ZipFile(directory / (stem + '.zip'), 'w') as kit:
        kit.writestr(stem + '/shin-kit.json', json.dumps(manifest))
        for p in paths:
            kit.writestr(stem + '/' + p, payload)
    (directory / (stem + '.msi')).write_bytes(bytes.fromhex('d0cf11e0a1b11ae1') + b'synthetic MSI header')
    for suffix in ('.zip', '.msi'):
        path = directory / (stem + suffix)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        (directory / (path.name + '.sha256')).write_text(f'{digest}  {path.name}\n')
    assets = {p.name: 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}
    receipt = dict(schema_version='shin.windows-qualification.v1', version=VERSION, core_version='2.0.5-dev.3',
                   render_scope='headless',
                   source_sha=SHA, run_id=123, attempt=attempt, assets=assets,
                   checks=['msi-metadata', 'msi-payload', 'archive-inventory', 'unelevated-runtime', 'installed-update-preflight'])
    (directory / 'windows-qualification.json').write_text(json.dumps(receipt))


def linux_kit(directory, attempt=1):
    import linux_package as linux
    directory.mkdir(parents=True)
    name = f'shin-terminal-{VERSION}-linux-x86_64'
    archive = directory / (name + '.tar.gz')
    payload = {path: b'independent synthetic Linux payload' for path in linux.REQUIRED_FILES}
    payload['share/shin-context/VERSION'] = b'2.0.5-dev.3\n'
    payload['share/shin-observer/VERSION'] = (VERSION+'\n').encode()
    manifest = dict(schema_version='shin.terminal-kit.v1', version=VERSION, core_version='2.0.5-dev.3',
                    platform='linux', architecture='x86_64', runtime=linux.RUNTIME,
                    files=[dict(path=p,size=len(b),sha256=hashlib.sha256(b).hexdigest(),executable=p.startswith('bin/'))
                           for p,b in sorted(payload.items())])
    with tarfile.open(archive, 'w:gz') as kit:
        for path,value in [(linux.MANIFEST,json.dumps(manifest).encode()), *payload.items()]:
            item=tarfile.TarInfo(name+'/'+path);item.size=len(value);item.mode=0o755 if path.startswith('bin/') else 0o644
            kit.addfile(item,io.BytesIO(value))
    (directory/(archive.name+'.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    receipt=linux.qualification_receipt(archive,manifest,SHA,123,attempt,
        dict(system='linux',distribution='ubuntu',distribution_version='24.04',architecture='x86_64',glibc='2.39'),linux.CHECKS)
    (directory/'linux-qualification.json').write_text(json.dumps(receipt))


class GitHubFixture:
    def __init__(self, uncertain=False, lose_upload=False, platform='macos'):
        self.run, self.jobs, self.artifact = fixtures(platform)
        self.state = None
        self.calls = []
        self.payloads = {}
        self.artifact_payloads = {}
        self.history = {}
        self.uncertain, self.lose_upload = uncertain, lose_upload
        self.pending_steps = 0

    def command(self, *args):
        self.calls.append(args)
        if args[0] == 'git':
            return SHA + '\n'
        if args[:2] == ('gh', 'api'):
            endpoint = args[2]
            if '/commits/' in endpoint:
                return json.dumps({'sha': SHA})
            match = re.search(r'/runs/([0-9]+)(?:/attempts/([0-9]+))?', endpoint)
            attempt = int(match[2] or self.run['run_attempt'])
            run, source_jobs = self.history.get(attempt, (self.run, self.jobs))
            if '/jobs?' in endpoint:
                jobs = copy.deepcopy(source_jobs)
                if self.pending_steps:
                    self.pending_steps -= 1
                    next(j for j in jobs if j['name'] == 'native (macos-15, observer)')['steps'] = []
                return json.dumps(dict(total_count=len(jobs), jobs=jobs))
            if '/artifacts?' in endpoint:
                return json.dumps(dict(total_count=len(self.artifact), artifacts=self.artifact))
            return json.dumps(run)
        if args[:3] == ('gh', 'run', 'download'):
            name = args[args.index('--name') + 1]
            target = Path(args[args.index('--dir') + 1])
            if name in self.artifact_payloads:
                target.mkdir(parents=True)
                for path,value in self.artifact_payloads[name].items():(target/path).write_bytes(value)
                return ''
            if name.startswith('shin-desktop-windows-'):
                windows_kit(target, attempt=int(name.rsplit('-', 1)[1]))
            elif name.startswith('shin-terminal-linux-'):
                linux_kit(target, attempt=int(name.rsplit('-', 1)[1]))
            else:
                kit(target)
            self.artifact_payloads[name]={p.name:p.read_bytes() for p in target.iterdir()}
            return ''
        if args[:3] == ('gh', 'release', 'download'):
            name = args[args.index('--pattern') + 1]
            directory = Path(args[args.index('--dir') + 1])
            directory.mkdir(exist_ok=True)
            if (directory / name).exists():
                raise subprocess.CalledProcessError(1,args,stderr='file already exists')
            (directory / name).write_bytes(self.payloads[name])
            return ''
        if args[:3] == ('gh', 'release', 'create'):
            self.state = dict(isDraft=True, isPrerelease=True, targetCommitish=SHA, assets=[], url='fixture-release',
                              body=Path(args[args.index('--notes-file')+1]).read_text())
        elif args[:3] == ('gh', 'release', 'upload'):
            asset = Path(args[-1])
            if not self.lose_upload:
                self.payloads[asset.name] = asset.read_bytes()
                self.state['assets'].append(dict(name=asset.name, state='uploaded',
                    digest='sha256:' + hashlib.sha256(asset.read_bytes()).hexdigest()))
            if self.lose_upload:
                raise subprocess.CalledProcessError(1, args)
        elif args[:3] == ('gh', 'release', 'edit'):
            self.state['isDraft'] = False
            self.state['body'] = Path(args[args.index('--notes-file')+1]).read_text()
        else:
            raise AssertionError(args)
        if self.uncertain:
            raise subprocess.CalledProcessError(1, args)
        return ''

    def receipt(self, args, **kwargs):
        if args[:3] != ['gh', 'release', 'view']:
            raise AssertionError(args)
        return SimpleNamespace(returncode=0 if self.state else 1,
                               stdout=json.dumps(self.state) if self.state else '', stderr='release not found')


class Publication(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='shin-publish-source-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'observer').mkdir()
        (root / 'observer/VERSION').write_text(VERSION)
        (root / 'VERSION').write_text('2.0.5-dev.3')
        (root / 'docs/releases').mkdir(parents=True)
        (root / 'docs/releases' / (TAG + '.md')).write_text('Independent preview fixture.\n')
        replacement = patch.object(publisher, '__file__', str(root / 'tools/publish_preview.py'))
        replacement.start()
        self.addCleanup(replacement.stop)

    def check_gate(self, run, jobs, artifact):
        publisher.qualification(run, jobs, artifact, REPO, SHA, 123, 1)

    def test_matching_matrix_while_publisher_is_running(self):
        self.check_gate(*fixtures())

    def test_each_unfinished_failed_or_missing_job_refuses(self):
        for name in publisher.PLATFORMS['macos']['jobs']:
            for status, conclusion in [('queued', None), ('in_progress', None), ('completed', 'failure'),
                                       ('completed', 'cancelled'), ('completed', 'skipped')]:
                with self.subTest(name=name, status=status, conclusion=conclusion):
                    run, jobs, artifact = fixtures()
                    job = next(j for j in jobs if j['name'] == name)
                    job.update(status=status, conclusion=conclusion)
                    with self.assertRaises(ValueError):
                        self.check_gate(run, jobs, artifact)
            run, jobs, artifact = fixtures()
            with self.assertRaises(ValueError):
                self.check_gate(run, [j for j in jobs if j['name'] != name], artifact)

    def test_source_attempt_provenance_and_package_proof(self):
        changes = [lambda r,j,a: r.update(head_sha='c'*40), lambda r,j,a: r.update(run_attempt=2),
                   lambda r,j,a: r.update(event='pull_request'), lambda r,j,a: r.update(path='other.yml'),
                   lambda r,j,a: r.update(conclusion='cancelled'), lambda r,j,a: a[0].update(expired=True),
                   lambda r,j,a: a[0]['workflow_run'].update(head_sha='c'*40),
                   lambda r,j,a: a[0].update(name='shin-local-macos-ARM64-123-2'),
                   lambda r,j,a: r['head_repository'].update(full_name='different/repo'),
                   lambda r,j,a: j.append(copy.deepcopy(next(x for x in j if x['name']=='native (macos-15, core)'))),
                   lambda r,j,a: next(x for x in j if x['name']=='native (macos-15, observer)')['steps'].clear()]
        for change in changes:
            run, jobs, artifact = fixtures()
            change(run, jobs, artifact)
            with self.assertRaises(ValueError):
                self.check_gate(run, jobs, artifact)

    def test_each_platform_requires_only_its_own_complete_source_and_archive_proof(self):
        for platform, spec in publisher.PLATFORMS.items():
            run, jobs, artifacts = fixtures(platform)
            for job in jobs:
                if job['name'] in publisher.MATRIX - spec['jobs']:
                    job.update(status='queued', conclusion=None)
            publisher.qualification(run, jobs, artifacts, REPO, SHA, 123, 1, platform=platform)
            for name in spec['jobs']:
                for status, conclusion in [('queued', None), ('completed', 'failure'), ('completed', 'cancelled')]:
                    changed = copy.deepcopy(jobs)
                    next(j for j in changed if j['name'] == name).update(status=status, conclusion=conclusion)
                    with self.subTest(platform=platform, job=name, status=status):
                        with self.assertRaises(ValueError):
                            publisher.qualification(run, changed, artifacts, REPO, SHA, 123, 1, platform=platform)
            changed = copy.deepcopy(jobs)
            next(j for j in changed if j['name'] == spec['package_job'])['steps'][0]['conclusion'] = 'failure'
            with self.assertRaises(ValueError):
                publisher.qualification(run, changed, artifacts, REPO, SHA, 123, 1, platform=platform)
            for altered in ([], artifacts + artifacts):
                with self.assertRaises(ValueError):
                    publisher.qualification(run, jobs, altered, REPO, SHA, 123, 1, platform=platform)

    def test_windows_receipt_binds_packages_source_attempt_and_runtime(self):
        changes = [lambda a, r: r.update(attempt=2), lambda a, r: r.update(source_sha='c'*40),
                   lambda a, r: r.update(core_version='wrong'), lambda a, r: r['checks'].remove('unelevated-runtime'),
                   lambda a, r: r['checks'].remove('installed-update-preflight'),
                   lambda a, r: r.update(render_scope='unverified'),
                   lambda a, r: r.update(assets={}),
                   lambda a, r: (a / f'shin-desktop-{VERSION}-windows-AMD64.msi').write_bytes(b'changed')]
        for change in changes:
            with tempfile.TemporaryDirectory() as folder:
                assets = Path(folder) / 'assets'
                windows_kit(assets)
                self.assertEqual(set(publisher.verify_windows_assets(assets, VERSION, '2.0.5-dev.3', SHA, 123, 1)), publisher.platform_assets('windows', VERSION))
                path = assets / 'windows-qualification.json'
                receipt = json.loads(path.read_text())
                change(assets, receipt)
                path.write_text(json.dumps(receipt))
                with self.assertRaises(ValueError):
                    publisher.verify_windows_assets(assets, VERSION, '2.0.5-dev.3', SHA, 123, 1)

    def test_windows_archive_inventory_is_verified_beyond_receipt_hashes(self):
        for failure in ('extra-file', 'wrong-core', 'changed-file', 'unsafe-path', 'duplicate'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as folder:
                assets = Path(folder) / 'assets'
                windows_kit(assets)
                name = f'shin-desktop-{VERSION}-windows-AMD64.zip'
                archive = assets / name
                original = archive.read_bytes()
                with zipfile.ZipFile(io.BytesIO(original)) as before, zipfile.ZipFile(archive, 'w') as after:
                    for entry in before.infolist():
                        value = before.read(entry)
                        if failure == 'wrong-core' and entry.filename.endswith('/shin-kit.json'):
                            manifest = json.loads(value); manifest['core_version'] = 'wrong'
                            value = json.dumps(manifest).encode()
                        if failure == 'changed-file' and entry.filename.endswith('/bin/shin-ce.exe'):
                            value += b'changed'
                        after.writestr(entry, value)
                    prefix = f'shin-desktop-{VERSION}-windows-AMD64/'
                    if failure == 'extra-file': after.writestr(prefix + 'unlisted', b'extra')
                    if failure == 'unsafe-path': after.writestr(prefix + '../outside', b'unsafe')
                    if failure == 'duplicate':
                        with self.assertWarns(UserWarning):
                            after.writestr(prefix + 'bin/shin-ce.exe', b'duplicate')
                (assets / (name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + name + '\n')
                receipt_file = assets / 'windows-qualification.json'
                receipt = json.loads(receipt_file.read_text())
                receipt['assets'] = {p.name: 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in assets.iterdir() if p != receipt_file}
                receipt_file.write_text(json.dumps(receipt))
                with self.assertRaises(ValueError):
                    publisher.verify_windows_assets(assets, VERSION, '2.0.5-dev.3', SHA, 123, 1)

    def test_windows_gate_is_rechecked_before_making_release_visible(self):
        github = GitHubFixture(platform='windows')
        original = github.command
        def changed(*args):
            result = original(*args)
            if args[:3] == ('gh', 'release', 'upload'):
                next(j for j in github.jobs if j['name'] == 'windows (desktop)')['conclusion'] = 'failure'
            return result
        with patch.object(publisher, 'command', changed), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1, platform='windows')
        self.assertTrue(github.state['isDraft'])
        self.assertFalse(any(c[:3] == ('gh', 'release', 'edit') for c in github.calls))

    def test_gate_precedes_any_download_or_release_mutation(self):
        github = GitHubFixture()
        next(j for j in github.jobs if j['name']=='native (macos-15, core)').update(status='in_progress', conclusion=None)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertFalse(any(c[:2] == ('gh', 'release') or c[:3] == ('gh','run','download') for c in github.calls))

    def test_delayed_step_metadata_is_refetched_before_download(self):
        github = GitHubFixture()
        github.pending_steps = 2
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt), \
                patch.object(publisher.time, 'sleep') as sleep:
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
        self.assertEqual(sleep.call_count, 2)
        download = next(i for i,c in enumerate(github.calls) if c[:3] == ('gh','run','download'))
        self.assertEqual(sum(c[:2] == ('gh','api') and '/jobs?' in c[2] for c in github.calls[:download]), 4)
        self.assertEqual(sum(c[:3] == ('gh','release','create') for c in github.calls), 1)

    def test_missing_step_metadata_expires_without_download_or_mutation(self):
        github = GitHubFixture()
        github.pending_steps = 20
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt), \
                patch.object(publisher.time, 'sleep') as sleep:
            with self.assertRaises(publisher.PackageProofPending):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertEqual(sleep.call_count, 9)
        self.assertFalse(any(c[:2] == ('gh','release') or c[:3] == ('gh','run','download') for c in github.calls))

    def test_failed_package_proof_is_not_retried(self):
        github = GitHubFixture()
        next(j for j in github.jobs if j['name'] == 'native (macos-15, observer)')['steps'][0]['conclusion'] = 'failure'
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt), \
                patch.object(publisher.time, 'sleep') as sleep:
            with self.assertRaisesRegex(ValueError, 'Missing package proof'):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        sleep.assert_not_called()
        self.assertFalse(any(c[:2] == ('gh','release') or c[:3] == ('gh','run','download') for c in github.calls))

    def test_failed_unrelated_platform_does_not_block_and_cancelled_run_does(self):
        run, jobs, artifact = fixtures()
        run.update(status='completed', conclusion='failure')
        next(j for j in jobs if j['name'] == 'windows (native)')['conclusion'] = 'failure'
        self.check_gate(run, jobs, artifact)
        run['conclusion'] = 'cancelled'
        with self.assertRaises(ValueError):
            self.check_gate(run, jobs, artifact)

    def test_failed_platform_publisher_requires_explicit_recovery(self):
        run, jobs, artifact = fixtures()
        run.update(status='completed', conclusion='failure')
        next(j for j in jobs if j['name'] == 'publish-preview (macos)').update(status='completed', conclusion='failure')
        with self.assertRaises(ValueError):
            self.check_gate(run, jobs, artifact)
        publisher.qualification(run, jobs, artifact, REPO, SHA, 123, 1, recover_publisher=True)
        next(j for j in jobs if j['name']=='native (macos-15, core)')['conclusion']='failure'
        with self.assertRaises(ValueError):
            publisher.qualification(run, jobs, artifact, REPO, SHA, 123, 1, recover_publisher=True)

    def test_source_tag_is_required_before_download(self):
        github = GitHubFixture()
        github.run['head_branch'] = 'main'
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaisesRegex(ValueError, 'preview tag'):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertFalse(any(c[:2] == ('gh','release') or c[:3] == ('gh','run','download') for c in github.calls))

    def test_recovery_publishes_exact_qualified_source_without_rebuilding(self):
        github = GitHubFixture()
        github.run.update(status='completed', conclusion='failure')
        next(j for j in github.jobs if j['name']=='publish-preview (macos)').update(status='completed', conclusion='failure')
        source = Path(publisher.__file__).parents[1]
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1, source_root=source,
                                              recover_publisher=True), 'fixture-release')
        self.assertEqual(sum(c[:3] == ('gh','run','download') for c in github.calls), 1)
        self.assertIn(('git', '-C', str(source.resolve()), 'rev-parse', 'HEAD'), github.calls)

    def test_uncertain_mutations_are_read_back_without_replay(self):
        github = GitHubFixture(uncertain=True)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
            before = len(github.calls)
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
        self.assertEqual(sum(c[:3] == ('gh','release','create') for c in github.calls), 1)
        self.assertEqual(sum(c[:3] == ('gh','release','upload') for c in github.calls), 6)
        self.assertEqual(sum(c[:3] == ('gh','release','edit') for c in github.calls), 1)
        self.assertFalse(any(c[:3] in (('gh','release','upload'), ('gh','release','edit'), ('gh','release','create')) for c in github.calls[before:]))

    def test_unconfirmed_upload_keeps_draft(self):
        github = GitHubFixture(lose_upload=True)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertTrue(github.state['isDraft'])
        self.assertEqual(sum(c[:3] == ('gh','release','upload') for c in github.calls), 1)
        self.assertFalse(any(c[:3] == ('gh','release','edit') for c in github.calls))

    def test_changed_existing_release_is_never_overwritten(self):
        github = GitHubFixture()
        github.state = dict(isDraft=True, isPrerelease=True, targetCommitish=SHA,
                            assets=[dict(name='foreign', state='uploaded', digest='sha256:'+'a'*64)])
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertFalse(any(c[:2] == ('gh','release') for c in github.calls))

    def test_archive_checksum_and_manifest_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            assets = Path(folder) / 'assets'
            kit(assets)
            publisher.verify_assets(assets, VERSION, '2.0.5-dev.3')
            archive = assets / f'shin-local-{VERSION}-macos-arm64.zip'
            archive.write_bytes(archive.read_bytes() + b'changed')
            with self.assertRaises(ValueError):
                publisher.verify_assets(assets, VERSION, '2.0.5-dev.3')

    def test_updater_must_be_complete_and_match_the_qualified_kit(self):
        for failure in ('missing', 'checksum', 'different-payload'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as folder:
                assets = Path(folder) / 'assets'
                kit(assets)
                updater = assets / 'shin-updater-v1-macos-arm64.zip'
                if failure == 'missing':
                    updater.unlink()
                elif failure == 'checksum':
                    updater.write_bytes(updater.read_bytes() + b'changed')
                else:
                    original = updater.read_bytes()
                    with zipfile.ZipFile(io.BytesIO(original)) as before, zipfile.ZipFile(updater, 'w') as after:
                        for entry in before.infolist():
                            payload = before.read(entry)
                            after.writestr(entry, b'changed updater' if entry.filename.endswith('/bin/shin-updater') else payload)
                    checksum = hashlib.sha256(updater.read_bytes()).hexdigest()
                    (assets / (updater.name + '.sha256')).write_text(f'{checksum}  {updater.name}\n')
                with self.assertRaises(ValueError):
                    publisher.verify_assets(assets, VERSION, '2.0.5-dev.3')

    def publish_with(self, github, platform='macos'):
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            return publisher.publish(REPO, SHA, TAG, 123, github.run['run_attempt'], platform=platform)

    def test_windows_publishes_while_arm_is_queued_then_macos_appends_immutably(self):
        github = GitHubFixture(platform='windows')
        for job in github.jobs:
            if job['name'].startswith('native (macos'):
                job.update(status='queued', conclusion=None)
        self.assertEqual(self.publish_with(github, 'windows'), 'fixture-release')
        previous = copy.deepcopy(github.state['assets'])
        self.assertFalse(github.state['isDraft'])
        self.assertTrue(set(publisher.platform_assets('windows', VERSION)) <= set(github.payloads))
        self.assertFalse(any('macos-arm64.zip' == n for n in github.payloads))
        for job in github.jobs:
            if job['name'].startswith('native (macos'):
                job.update(status='completed', conclusion='success')
            if job['name']=='publish-preview (macos)':job.update(status='in_progress', conclusion=None)
        github.artifact += fixtures('macos')[2]
        self.assertEqual(self.publish_with(github), 'fixture-release')
        self.assertTrue(all(asset in github.state['assets'] for asset in previous))
        self.assertEqual(sum(c[:3] == ('gh','release','create') for c in github.calls), 1)
        self.assertEqual(len(github.state['assets']), 12)

    def test_platform_marker_preserves_previous_successful_attempt(self):
        github = GitHubFixture(platform='windows')
        self.publish_with(github, 'windows')
        previous = copy.deepcopy(github.state['assets'])
        github.history[1] = (copy.deepcopy(github.run), copy.deepcopy(github.jobs))
        github.run, github.jobs, mac_artifacts = fixtures('macos', attempt=2)
        github.artifact += mac_artifacts
        self.assertEqual(self.publish_with(github), 'fixture-release')
        self.assertTrue(all(asset in github.state['assets'] for asset in previous))
        self.assertEqual(json.loads(github.payloads['shin-preview-windows.json'])['attempt'], 1)
        self.assertEqual(json.loads(github.payloads['shin-preview-macos.json'])['attempt'], 2)

    def test_macos_zip_is_last_payload_before_platform_marker(self):
        github = GitHubFixture()
        self.publish_with(github)
        names=[Path(c[-1]).name for c in github.calls if c[:3]==('gh','release','upload')]
        self.assertEqual(names[-2:], [f'shin-local-{VERSION}-macos-arm64.zip', 'shin-preview-macos.json'])
        self.assertTrue(all(name in names[:-2] for name in ('install.sh','shin-updater-v1-macos-arm64.zip',
                                                         'shin-updater-v1-macos-arm64.zip.sha256')))

    def test_changed_foreign_platform_or_receipt_is_never_accepted(self):
        for failure in ('digest', 'marker'):
            github=GitHubFixture(platform='windows');self.publish_with(github,'windows')
            before=len(github.calls)
            github.jobs[-2].update(status='in_progress',conclusion=None)
            github.artifact += fixtures('macos')[2]
            if failure=='digest':github.state['assets'][0]['digest']='sha256:'+'c'*64
            else:
                name='shin-preview-windows.json'
                value=json.loads(github.payloads[name]);value['assets'][next(iter(value['assets']))]='sha256:'+'c'*64
                github.payloads[name]=publisher.marker_bytes(value)
            with self.assertRaises(ValueError):self.publish_with(github)
            self.assertFalse(any(c[:3] in (('gh','release','upload'),('gh','release','edit')) for c in github.calls[before:]))


    def test_linux_publishes_without_any_macos_or_windows_runner(self):
        github=GitHubFixture(platform='linux')
        for job in github.jobs:
            if job['name'] in publisher.MATRIX - publisher.PLATFORMS['linux']['jobs']:
                job.update(status='queued',conclusion=None)
        self.assertEqual(self.publish_with(github,'linux'),'fixture-release')
        self.assertEqual(set(github.payloads),publisher.platform_assets('linux',VERSION)|{'shin-preview-linux.json'})

    def test_incomplete_other_platform_does_not_block_qualified_windows(self):
        github=GitHubFixture()
        original=github.command
        uploads=0
        def interrupted(*args):
            nonlocal uploads
            if args[:3]==('gh','release','upload'):
                uploads+=1
                if uploads==2:raise subprocess.CalledProcessError(1,args)
            return original(*args)
        with patch.object(publisher,'command',interrupted),patch.object(publisher.subprocess,'run',github.receipt):
            with self.assertRaises(ValueError):publisher.publish(REPO,SHA,TAG,123,1)
        self.assertTrue(github.state['isDraft'])
        self.assertNotIn(f'shin-local-{VERSION}-macos-arm64.zip',github.payloads)
        github.artifact += fixtures('windows')[2]
        next(j for j in github.jobs if j['name']=='publish-preview (windows)').update(status='in_progress',conclusion=None)
        self.assertEqual(self.publish_with(github,'windows'),'fixture-release')
        self.assertFalse(github.state['isDraft'])
        self.assertTrue(publisher.platform_assets('windows',VERSION)<=set(github.payloads))
        self.assertNotIn(f'shin-local-{VERSION}-macos-arm64.zip',github.payloads)

    def test_linux_proof_requires_exact_runtime_and_all_checks(self):
        with tempfile.TemporaryDirectory() as folder:
            directory=Path(folder)/'assets';linux_kit(directory)
            publisher.verify_linux_assets(directory,VERSION,'2.0.5-dev.3',SHA,123,1)
            receipt_path=directory/'linux-qualification.json';original=json.loads(receipt_path.read_text())
            for change in (lambda r:r['checks'].pop(),lambda r:r.update(attempt=2),
                           lambda r:r['runtime'].update(distribution_version='22.04')):
                value=copy.deepcopy(original);change(value);receipt_path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):publisher.verify_linux_assets(directory,VERSION,'2.0.5-dev.3',SHA,123,1)

    def test_workflow_publishers_have_independent_dependencies_and_one_shared_queue(self):
        root=Path(__file__).resolve().parents[1]
        text=(root/'.github/workflows/ci.yml').read_text()
        events=text.split('\npermissions:',1)[0]
        push=re.search(r'^  push:\n(.*?)(?=^  [a-zA-Z]|\Z)',events,re.M|re.S)
        self.assertIsNotNone(push)
        self.assertEqual(push[1].strip(), "tags:\n      - 'v*'")
        self.assertRegex(events,r'(?m)^  pull_request:\s*$')
        self.assertRegex(events,r'(?m)^  workflow_dispatch:\s*$')
        for target in publisher.PLATFORMS:
            match=re.search(r'^  publish-'+target+r':\n(.*?)(?=^  [a-zA-Z]|\Z)',text,re.M|re.S)
            self.assertIsNotNone(match,target)
            block=match[1]
            self.assertIn('needs: ['+target+']',block)
            self.assertIn('--platform '+target,block)
            self.assertIn('!cancelled()',block)
            self.assertIn('group: preview-assets-${{ github.ref }}',block)
            self.assertIn('queue: max',block)
            self.assertNotIn('cancel-in-progress: true',block)
        recovery=(root/'.github/workflows/publish-preview.yml').read_text()
        self.assertIn('group: preview-assets-refs/tags/${{ inputs.tag }}',recovery)
        self.assertIn('queue: max',recovery)
        self.assertIn('--platform "$PREVIEW_PLATFORM"',recovery)

    def test_cancellation_or_tag_movement_stops_public_platform_append_before_kit_visibility(self):
        for failure in ('cancelled','moved-tag'):
            github=GitHubFixture(platform='windows');self.publish_with(github,'windows')
            github.artifact += fixtures('macos')[2]
            next(j for j in github.jobs if j['name']=='publish-preview (macos)').update(status='in_progress',conclusion=None)
            original=github.command;changed=False;before=len(github.calls)
            def cancel(*args):
                nonlocal changed
                if changed and args[:2]==('gh','api') and '/commits/' in args[2] and failure=='moved-tag':
                    github.calls.append(args);return json.dumps({'sha':'c'*40})
                value=original(*args)
                if args[:3]==('gh','release','upload'):
                    changed=True
                    if failure=='cancelled':github.run.update(status='completed',conclusion='cancelled')
                return value
            with patch.object(publisher,'command',cancel),patch.object(publisher.subprocess,'run',github.receipt):
                with self.assertRaises(ValueError):publisher.publish(REPO,SHA,TAG,123,1)
            self.assertFalse(github.state['isDraft'])
            self.assertNotIn(f'shin-local-{VERSION}-macos-arm64.zip',github.payloads)
            self.assertNotIn('shin-preview-macos.json',github.payloads)
            self.assertEqual(sum(c[:3]==('gh','release','upload') for c in github.calls[before:]),1)

    def test_failed_notes_append_is_detected_and_completed_assets_can_repair_it(self):
        github=GitHubFixture(platform='windows');self.publish_with(github,'windows')
        previous=github.state['body']
        github.artifact += fixtures('macos')[2]
        next(j for j in github.jobs if j['name']=='publish-preview (macos)').update(status='in_progress',conclusion=None)
        original=github.command
        def lose_edit(*args):
            if args[:3]==('gh','release','edit'):
                github.calls.append(args);raise subprocess.CalledProcessError(1,args)
            return original(*args)
        with patch.object(publisher,'command',lose_edit),patch.object(publisher.subprocess,'run',github.receipt):
            with self.assertRaisesRegex(ValueError,'notes update did not confirm'):
                publisher.publish(REPO,SHA,TAG,123,1)
        self.assertEqual(github.state['body'],previous)
        self.assertIn('shin-preview-macos.json',github.payloads)
        before=len(github.calls)
        self.assertEqual(self.publish_with(github),'fixture-release')
        self.assertIn('| macos | Qualified downloads available |',github.state['body'])
        self.assertFalse(any(c[:3]==('gh','release','upload') for c in github.calls[before:]))


if __name__ == '__main__':
    unittest.main()
