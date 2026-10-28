#!/usr/bin/env python3
"""Offline publication gates and uncertain-outcome readback; no GitHub mutations."""
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

import publish_preview as publisher

SHA = 'b' * 40
REPO = 'fixture/shin'
VERSION = '2.0.5-dev.21'
TAG = 'v' + VERSION


def fixtures():
    run = dict(id=123, head_sha=SHA, head_branch=TAG, run_attempt=1, event='push', path='.github/workflows/ci.yml',
               repository={'full_name': REPO}, head_repository={'full_name': REPO},
               status='in_progress', conclusion=None)
    jobs = [dict(name=name, status='completed', conclusion='success', steps=[])
            for name in sorted(publisher.MATRIX)]
    next(j for j in jobs if j['name'] == 'native (macos-15, observer)')['steps'] = [
        dict(name=name, conclusion='success') for name in
        ('Exact macOS kit qualification', 'Upload qualified macOS kit')]
    next(j for j in jobs if j['name'] == 'windows (desktop)')['steps'] = [
        dict(name=name, conclusion='success') for name in
        ('Exact Windows kit qualification', 'Upload qualified Windows kit')]
    artifacts = [dict(name=name, expired=False, workflow_run={'id': 123, 'head_sha': SHA})
                 for name in sorted(publisher.artifact_names(123, 1))]
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


def windows_kit(directory):
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
                   source_sha=SHA, run_id=123, attempt=1, assets=assets,
                   checks=['msi-metadata', 'msi-payload', 'archive-inventory', 'unelevated-runtime'])
    (directory / 'windows-qualification.json').write_text(json.dumps(receipt))


class GitHubFixture:
    def __init__(self, uncertain=False, lose_upload=False):
        self.run, self.jobs, self.artifact = fixtures()
        self.state = None
        self.calls = []
        self.uncertain, self.lose_upload = uncertain, lose_upload
        self.pending_steps = 0

    def command(self, *args):
        self.calls.append(args)
        if args[0] == 'git':
            return SHA + '\n'
        if args[:2] == ('gh', 'api'):
            endpoint = args[2]
            if '/jobs?' in endpoint:
                jobs = copy.deepcopy(self.jobs)
                if self.pending_steps:
                    self.pending_steps -= 1
                    next(j for j in jobs if j['name'] == 'native (macos-15, observer)')['steps'] = []
                return json.dumps(dict(total_count=len(jobs), jobs=jobs))
            if '/artifacts?' in endpoint:
                return json.dumps(dict(total_count=len(self.artifact), artifacts=self.artifact))
            if '/commits/' in endpoint:
                return json.dumps({'sha': SHA})
            return json.dumps(self.run)
        if args[:3] == ('gh', 'run', 'download'):
            make = windows_kit if args[args.index('--name') + 1].startswith('shin-desktop-windows-') else kit
            make(Path(args[args.index('--dir') + 1]))
            return ''
        if args[:3] == ('gh', 'release', 'create'):
            self.state = dict(isDraft=True, isPrerelease=True, targetCommitish=SHA, assets=[], url='fixture-release')
        elif args[:3] == ('gh', 'release', 'upload'):
            asset = Path(args[-1])
            if not self.lose_upload:
                self.state['assets'].append(dict(name=asset.name, state='uploaded',
                    digest='sha256:' + hashlib.sha256(asset.read_bytes()).hexdigest()))
            if self.lose_upload:
                raise subprocess.CalledProcessError(1, args)
        elif args[:3] == ('gh', 'release', 'edit'):
            self.state['isDraft'] = False
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
        for name in publisher.MATRIX:
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
                   lambda r,j,a: r.update(conclusion='failure'), lambda r,j,a: a[0].update(expired=True),
                   lambda r,j,a: a[0]['workflow_run'].update(head_sha='c'*40),
                   lambda r,j,a: a[0].update(name='shin-local-macos-ARM64-123-2'),
                   lambda r,j,a: r['head_repository'].update(full_name='different/repo'),
                   lambda r,j,a: j.append(copy.deepcopy(j[0])),
                   lambda r,j,a: next(x for x in j if x['name']=='native (macos-15, observer)')['steps'].clear()]
        for change in changes:
            run, jobs, artifact = fixtures()
            change(run, jobs, artifact)
            with self.assertRaises(ValueError):
                self.check_gate(run, jobs, artifact)

    def test_both_platforms_and_windows_package_proof_are_mandatory(self):
        for change in (lambda a: a.pop(), lambda a: a.append(copy.deepcopy(a[0])),
                       lambda a: a[0].update(expired=True), lambda a: a[1].update(expired=True)):
            run, jobs, artifacts = fixtures()
            change(artifacts)
            with self.assertRaises(ValueError):
                self.check_gate(run, jobs, artifacts)
        run, jobs, artifacts = fixtures()
        next(j for j in jobs if j['name'] == 'windows (desktop)')['steps'][0]['conclusion'] = 'failure'
        with self.assertRaises(ValueError):
            self.check_gate(run, jobs, artifacts)

    def test_windows_receipt_binds_packages_source_attempt_and_runtime(self):
        changes = [lambda a, r: r.update(attempt=2), lambda a, r: r.update(source_sha='c'*40),
                   lambda a, r: r.update(core_version='wrong'), lambda a, r: r['checks'].pop(),
                   lambda a, r: r.update(assets={}),
                   lambda a, r: (a / f'shin-desktop-{VERSION}-windows-AMD64.msi').write_bytes(b'changed')]
        for change in changes:
            with tempfile.TemporaryDirectory() as folder:
                assets = Path(folder) / 'assets'
                windows_kit(assets)
                self.assertEqual(len(publisher.verify_windows_assets(assets, VERSION, '2.0.5-dev.3', SHA, 123, 1)), 4)
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
        github = GitHubFixture()
        original = github.command
        def changed(*args):
            result = original(*args)
            if args[:3] == ('gh', 'release', 'upload'):
                next(j for j in github.jobs if j['name'] == 'windows (desktop)')['conclusion'] = 'failure'
            return result
        with patch.object(publisher, 'command', changed), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertTrue(github.state['isDraft'])
        self.assertFalse(any(c[:3] == ('gh', 'release', 'edit') for c in github.calls))

    def test_gate_precedes_any_download_or_release_mutation(self):
        github = GitHubFixture()
        github.jobs[0].update(status='in_progress', conclusion=None)
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
        self.assertEqual(sum(c[:2] == ('gh','api') and '/jobs?' in c[2] for c in github.calls[:download]), 3)
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

    def test_recovery_requires_only_publisher_failed_and_all_native_jobs_passed(self):
        run, jobs, artifact = fixtures()
        run.update(status='completed', conclusion='failure')
        jobs.append(dict(name='publish-preview', status='completed', conclusion='failure'))
        with self.assertRaises(ValueError):
            self.check_gate(run, jobs, artifact)
        publisher.qualification(run, jobs, artifact, REPO, SHA, 123, 1, recover_publisher=True)
        for name in publisher.MATRIX:
            changed = copy.deepcopy(jobs)
            next(j for j in changed if j['name'] == name)['conclusion'] = 'failure'
            with self.assertRaises(ValueError):
                publisher.qualification(run, changed, artifact, REPO, SHA, 123, 1, recover_publisher=True)
        for change in [lambda j: j.pop(), lambda j: j.append(dict(name='foreign', conclusion='failure')),
                       lambda j: j[-1].update(status='in_progress', conclusion=None)]:
            changed = copy.deepcopy(jobs)
            change(changed)
            with self.assertRaises(ValueError):
                publisher.qualification(run, changed, artifact, REPO, SHA, 123, 1, recover_publisher=True)

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
        github.jobs.append(dict(name='publish-preview', status='completed', conclusion='failure'))
        source = Path(publisher.__file__).parents[1]
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1, source_root=source,
                                              recover_publisher=True), 'fixture-release')
        self.assertEqual(sum(c[:3] == ('gh','run','download') for c in github.calls), 2)
        self.assertIn(('git', '-C', str(source.resolve()), 'rev-parse', 'HEAD'), github.calls)

    def test_uncertain_mutations_are_read_back_without_replay(self):
        github = GitHubFixture(uncertain=True)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
            before = len(github.calls)
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
        self.assertEqual(sum(c[:3] == ('gh','release','create') for c in github.calls), 1)
        self.assertEqual(sum(c[:3] == ('gh','release','upload') for c in github.calls), 9)
        self.assertEqual(sum(c[:3] == ('gh','release','edit') for c in github.calls), 1)
        self.assertFalse(any(c[:2] == ('gh','release') for c in github.calls[before:]))

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


if __name__ == '__main__':
    unittest.main()
