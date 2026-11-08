#!/usr/bin/env python3
"""Compiler data must not cross an unqualified run or escape its cache directory."""
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import ci_compiler_cache as cache


class CompilerCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-compiler-cache-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.ident = dict(platform='macos', arch='arm64', component='observer', engine='ccache',
                          engine_version='fixture-cache', compiler_sha256='c' * 64, configuration='Release')
        self.source = dict(repository='fixture/shin', sha='a' * 40, tag='v1.2.3-dev.4',
                           run_id=17, attempt=2, job='native (macos-15, observer)')
        self.name = 'fixture-cache-artifact'
        self.artifact = dict(id=71, name=self.name, expired=False, size_in_bytes=512,
                             workflow_run=dict(id=17, head_sha=self.source['sha']))
        self.run = dict(id=17, head_sha=self.source['sha'], head_branch=self.source['tag'],
                        run_attempt=2, repository=dict(full_name='fixture/shin'),
                        head_repository=dict(full_name='fixture/shin'), event='push',
                        path='.github/workflows/ci.yml', conclusion=None, status='in_progress')
        self.job = dict(name=self.source['job'], status='completed', conclusion='success', steps=[
            dict(name=name, conclusion='success') for name in (
                'Build', 'Tests', 'Exact macOS kit qualification', 'Upload qualified macOS kit', 'Upload compiler cache')])
        self.tag = dict(object=dict(type='commit', sha=self.source['sha']))

    def fetch(self, route):
        if 'actions/artifacts?' in route:
            return dict(artifacts=[self.artifact])
        if route == 'repos/fixture/shin/actions/runs/17':
            return self.run
        if route == 'repos/fixture/shin/actions/runs/17/attempts/2/jobs?per_page=100':
            # Unrelated failures cannot block a qualified platform's cache.
            return dict(jobs=[self.job, dict(name='windows (desktop)', status='completed', conclusion='failure')])
        if route == 'repos/fixture/shin/git/ref/tags/v1.2.3-dev.4':
            return self.tag
        if route == 'repos/fixture/shin/git/tags/' + 'b' * 40:
            return dict(object=dict(type='commit', sha=self.source['sha']))
        raise KeyError(route)

    def select(self):
        return cache.select(self.fetch, 'fixture/shin', self.name, self.ident, 99)

    def state(self):
        return dict(identity=self.ident, artifact_name=self.name, origin=self.source,
                    selected=dict(source=self.source, artifact_id=71))

    def bundle(self):
        directory = self.base / 'producer-cache'
        directory.mkdir()
        (directory / '0').mkdir()
        (directory / '0/object').write_bytes(b'opaque compiler result')
        bundle = self.base / 'bundle'
        cache.export_cache(directory, bundle, self.state())
        return bundle

    def rewrite_archive(self, bundle, members):
        archive = bundle / 'compiler-cache.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            for name, kind in members:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.linkname = '../foreign'
                member.size = 1 if kind == tarfile.REGTYPE else 0
                tar.addfile(member, io.BytesIO(b'x') if member.size else None)
        metadata = json.loads((bundle / 'compiler-cache.json').read_text())
        metadata['archive'].update(bytes=archive.stat().st_size, sha256=cache.sha256(archive),
                                   files=len(members), content_bytes=len(members))
        (bundle / 'compiler-cache.json').write_text(json.dumps(metadata))

    def test_qualified_component_is_reused_without_waiting_for_other_platforms(self):
        self.assertEqual(self.select()['source'], self.source)

    def test_annotated_tag_is_peeled_and_bound_to_source(self):
        self.tag['object'] = dict(type='tag', sha='b' * 40)
        self.assertEqual(self.select()['source'], self.source)

    def test_untrusted_or_incomplete_sources_are_never_selected(self):
        variants = [
            ('run', 'event', 'pull_request'), ('run', 'path', '.github/workflows/other.yml'),
            ('run', 'head_repository', dict(full_name='other/shin')), ('run', 'conclusion', 'cancelled'),
            ('run', 'head_sha', 'f' * 40), ('run', 'head_branch', 'main'), ('run', 'run_attempt', 3),
            ('artifact', 'expired', True), ('artifact', 'size_in_bytes', cache.MAX_ARCHIVE + 65537),
            ('job', 'status', 'in_progress'), ('job', 'conclusion', 'failure')]
        for obj, key, value in variants:
            with self.subTest(obj=obj, key=key):
                before = copy.deepcopy(getattr(self, obj))
                getattr(self, obj)[key] = value
                self.assertIsNone(self.select())
                setattr(self, obj, before)
        self.assertIsNone(cache.select(self.fetch, 'fixture/shin', self.name, self.ident, 17))

    def test_retargeted_tag_and_missing_package_check_are_rejected(self):
        self.tag['object']['sha'] = 'f' * 40
        self.assertIsNone(self.select())
        self.tag['object']['sha'] = self.source['sha']
        self.job['steps'][2]['conclusion'] = 'skipped'
        self.assertIsNone(self.select())

    def test_only_trusted_tag_events_can_export(self):
        env = dict(GITHUB_REPOSITORY='fixture/shin', GITHUB_REF_NAME=self.source['tag'],
                   GITHUB_REF_TYPE='tag', GITHUB_EVENT_NAME='push', GITHUB_SHA=self.source['sha'],
                   GITHUB_RUN_ID='17', GITHUB_RUN_ATTEMPT='2')
        self.assertEqual(cache.origin(env, self.ident), self.source)
        for change in (dict(GITHUB_EVENT_NAME='pull_request'), dict(GITHUB_REF_TYPE='branch'),
                       dict(GITHUB_REF_NAME='v1.2.3-dev.04'), dict(GITHUB_SHA='not-a-sha')):
            with self.subTest(change=change), self.assertRaises(ValueError):
                cache.origin({**env, **change}, self.ident)

    def test_identity_survives_release_tags_but_separates_compilers_and_components(self):
        compiler = self.base / 'compiler'
        compiler.write_bytes(b'compiler one')
        env = dict(RUNNER_OS='macOS', RUNNER_ARCH='ARM64', GITHUB_REF_NAME='v1.2.3-dev.4')
        with patch.object(cache.subprocess, 'check_output', return_value='ccache fixture\n'):
            first = cache.identity('ccache', compiler, 'observer', env)
            self.assertEqual(first, cache.identity('ccache', compiler, 'observer',
                                                  {**env, 'GITHUB_REF_NAME': 'v1.2.3-dev.5'}))
            self.assertNotEqual(first[1], cache.identity('ccache', compiler, 'core', env)[1])
            compiler.write_bytes(b'compiler two')
            self.assertNotEqual(first[1], cache.identity('ccache', compiler, 'observer', env)[1])

    def test_expired_api_budget_does_not_make_a_network_request(self):
        api = cache.Api('fixture-token')
        api.deadline = 0
        with patch.object(api.opener, 'open') as request, self.assertRaises(ValueError):
            api('repos/fixture/shin/actions/artifacts')
        request.assert_not_called()

    def test_msvc_codegen_change_invalidates_the_cache_even_if_cl_is_unchanged(self):
        compiler = self.base / 'cl.exe'
        compiler.write_bytes(b'compiler dispatcher')
        for name in ('c1.dll', 'c1xx.dll', 'c2.dll'):
            (self.base / name).write_bytes(b'compiler implementation one')
        env = dict(RUNNER_OS='Windows', RUNNER_ARCH='X64')
        with patch.object(cache.subprocess, 'check_output', return_value='sccache fixture\n'):
            first = cache.identity('sccache', compiler, 'native', env)
            (self.base / 'c2.dll').write_bytes(b'compiler implementation two')
            second = cache.identity('sccache', compiler, 'native', env)
        self.assertEqual(first[0]['compiler_sha256'], second[0]['compiler_sha256'])
        self.assertNotEqual(first[1], second[1])

    def test_opaque_cache_round_trip_preserves_bytes(self):
        bundle = self.bundle()
        destination = self.base / 'consumer-cache'
        result = cache.restore_cache(bundle, destination, self.state())
        self.assertEqual(result['files'], 1)
        self.assertEqual((destination / '0/object').read_bytes(), b'opaque compiler result')

    def test_action_outputs_choose_restored_data_only_after_a_valid_import(self):
        bundle = self.bundle()
        state = self.base / 'state.json'
        state.write_text(json.dumps(self.state()))
        outputs = self.base / 'outputs'
        command = [sys.executable, '-B', str(Path(cache.__file__)), 'restore', '--state', str(state),
                   '--bundle', str(bundle), '--cache', str(self.base / 'consumer')]
        env = {**os.environ, 'GITHUB_OUTPUT': str(outputs)}
        first = subprocess.run(command, env=env, capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(first.stdout)['status'], 'restored')
        self.assertEqual(outputs.read_text(), 'restored=true\n')
        outputs.unlink()
        second = subprocess.run(command, env=env, capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(second.stdout)['status'], 'miss')
        self.assertFalse(outputs.exists())

    def test_wrong_compiler_or_attempt_never_restores_data(self):
        bundle = self.bundle()
        for field, value in (('identity', {**self.ident, 'compiler_sha256': 'f' * 64}),
                             ('source', {**self.source, 'attempt': 1})):
            metadata = json.loads((bundle / 'compiler-cache.json').read_text())
            original = copy.deepcopy(metadata)
            metadata[field] = value
            (bundle / 'compiler-cache.json').write_text(json.dumps(metadata))
            with self.subTest(field=field), self.assertRaises(ValueError):
                cache.restore_cache(bundle, self.base / 'consumer', self.state())
            self.assertFalse((self.base / 'consumer').exists())
            (bundle / 'compiler-cache.json').write_text(json.dumps(original))

    def test_tampered_archive_and_nonempty_destination_are_preserved(self):
        bundle = self.bundle()
        target = self.base / 'consumer'
        target.mkdir()
        (target / 'keep').write_bytes(b'user data')
        with self.assertRaises(ValueError):
            cache.restore_cache(bundle, target, self.state())
        self.assertEqual((target / 'keep').read_bytes(), b'user data')
        archive = bundle / 'compiler-cache.tar.gz'
        archive.write_bytes(archive.read_bytes() + b'changed')
        with self.assertRaises(ValueError):
            cache.restore_cache(bundle, self.base / 'new-consumer', self.state())
        self.assertFalse((self.base / 'new-consumer').exists())

    def test_unsafe_tar_members_never_escape_or_partially_replace_the_cache(self):
        bundle = self.bundle()
        for members in ([('../escape', tarfile.REGTYPE)], [('/escape', tarfile.REGTYPE)],
                        [('a\\escape', tarfile.REGTYPE)], [('C:escape', tarfile.REGTYPE)],
                        [('link', tarfile.SYMTYPE)], [('link', tarfile.LNKTYPE)],
                        [('duplicate', tarfile.REGTYPE), ('duplicate', tarfile.REGTYPE)]):
            with self.subTest(members=members):
                self.rewrite_archive(bundle, members)
                with self.assertRaises(ValueError):
                    cache.restore_cache(bundle, self.base / 'consumer', self.state())
                self.assertFalse((self.base / 'consumer').exists())
                self.assertFalse((self.base / 'escape').exists())

    def test_optional_cache_failure_cannot_claim_a_successful_build(self):
        result = subprocess.run([sys.executable, str(Path(cache.__file__)), 'restore',
                                 '--state', str(self.base / 'absent.json')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        report = json.loads(result.stdout)
        self.assertEqual(report['status'], 'miss')
        self.assertFalse(result.stderr)
        self.assertNotIn('qualification', report)


if __name__ == '__main__':
    unittest.main()
