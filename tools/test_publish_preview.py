#!/usr/bin/env python3
"""Offline publication gates and uncertain-outcome readback; no GitHub mutations."""
import copy
import hashlib
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
    run = dict(id=123, head_sha=SHA, run_attempt=1, event='push', path='.github/workflows/ci.yml',
               repository={'full_name': REPO}, head_repository={'full_name': REPO},
               status='in_progress', conclusion=None)
    jobs = [dict(name=name, status='completed', conclusion='success', steps=[])
            for name in sorted(publisher.MATRIX)]
    next(j for j in jobs if j['name'] == 'native (macos-15, observer)')['steps'] = [
        dict(name=name, conclusion='success') for name in
        ('Exact macOS kit qualification', 'Upload qualified macOS kit')]
    artifact = dict(name='shin-local-macos-ARM64-123-1', expired=False,
                    workflow_run={'id': 123, 'head_sha': SHA})
    return run, jobs, artifact


def kit(directory):
    directory.mkdir(parents=True)
    name = f'shin-local-{VERSION}-macos-arm64.zip'
    data = b'independent synthetic kit payload'
    manifest = dict(schema_version='shin.local-kit.v1', version=VERSION, core_version='2.0.5-dev.3',
                    files=[dict(path='bin/fixture', size=len(data), sha256=hashlib.sha256(data).hexdigest())])
    with zipfile.ZipFile(directory / name, 'w') as archive:
        archive.writestr(f'shin-local-{VERSION}-macos-arm64/shin-kit.json', json.dumps(manifest))
        archive.writestr(f'shin-local-{VERSION}-macos-arm64/bin/fixture', data)
    digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    (directory / (name + '.sha256')).write_text(f'{digest}  {name}\n')
    (directory / 'install.sh').write_text(f"version='{VERSION}'\n")


class GitHubFixture:
    def __init__(self, uncertain=False, lose_upload=False):
        self.run, self.jobs, self.artifact = fixtures()
        self.state = None
        self.calls = []
        self.uncertain, self.lose_upload = uncertain, lose_upload

    def command(self, *args):
        self.calls.append(args)
        if args[0] == 'git':
            return SHA + '\n'
        if args[:2] == ('gh', 'api'):
            endpoint = args[2]
            if '/jobs?' in endpoint:
                return json.dumps(dict(total_count=len(self.jobs), jobs=self.jobs))
            if '/artifacts?' in endpoint:
                return json.dumps(dict(total_count=1, artifacts=[self.artifact]))
            if '/commits/' in endpoint:
                return json.dumps({'sha': SHA})
            return json.dumps(self.run)
        if args[:3] == ('gh', 'run', 'download'):
            kit(Path(args[args.index('--dir') + 1]))
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
                   lambda r,j,a: r.update(conclusion='failure'), lambda r,j,a: a.update(expired=True),
                   lambda r,j,a: a['workflow_run'].update(head_sha='c'*40),
                   lambda r,j,a: a.update(name='shin-local-macos-ARM64-123-2'),
                   lambda r,j,a: r['head_repository'].update(full_name='different/repo'),
                   lambda r,j,a: j.append(copy.deepcopy(j[0])),
                   lambda r,j,a: next(x for x in j if x['name']=='native (macos-15, observer)')['steps'].clear()]
        for change in changes:
            run, jobs, artifact = fixtures()
            change(run, jobs, artifact)
            with self.assertRaises(ValueError):
                self.check_gate(run, jobs, artifact)

    def test_gate_precedes_any_download_or_release_mutation(self):
        github = GitHubFixture()
        github.jobs[0].update(status='in_progress', conclusion=None)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            with self.assertRaises(ValueError):
                publisher.publish(REPO, SHA, TAG, 123, 1)
        self.assertFalse(any(c[:2] == ('gh', 'release') or c[:3] == ('gh','run','download') for c in github.calls))

    def test_uncertain_mutations_are_read_back_without_replay(self):
        github = GitHubFixture(uncertain=True)
        with patch.object(publisher, 'command', github.command), patch.object(publisher.subprocess, 'run', github.receipt):
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
            before = len(github.calls)
            self.assertEqual(publisher.publish(REPO, SHA, TAG, 123, 1), 'fixture-release')
        self.assertEqual(sum(c[:3] == ('gh','release','create') for c in github.calls), 1)
        self.assertEqual(sum(c[:3] == ('gh','release','upload') for c in github.calls), 3)
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


if __name__ == '__main__':
    unittest.main()
