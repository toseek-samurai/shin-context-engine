#!/usr/bin/env python3
"""Preview preparation and interrupted handoff with disposable Git repositories."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import create_preview as release


class Preparation(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='shin-preview-preparation-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / 'source'
        self.root.mkdir()
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.name', 'Synthetic Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.write('VERSION', '7.0.0-dev.3\n')
        self.write('observer/VERSION', '7.8.9-dev.4\n')
        self.policy = {'developmentLane': {'coreVersion': '7.0.0-dev.3',
                       'observerSourceVersion': '7.8.9-dev.4', 'untouched': {'value': 42}}}
        self.write(release.POLICY, json.dumps(self.policy, indent=2) + '\n')
        self.write('docs/releases/v7.8.9-dev.4.md', '# v7.8.9-dev.4\n\nPrevious fixture.\n')
        self.write('README.md', 'Independent synthetic source\n')
        self.commit('Previous preview')
        self.git('tag', 'v7.8.9-dev.4')
        self.origin = Path(temp.name) / 'origin.git'
        subprocess.run(['git', 'init', '-q', '--bare', str(self.origin)], check=True)
        self.git('remote', 'add', 'origin', str(self.origin))
        self.git('push', '-q', 'origin', 'main', '--tags')

    def git(self, *args):
        return release.git(self.root, *args)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self, message='Fixture change'):
        self.git('add', '.')
        self.git('commit', '-q', '-m', message)

    def change(self):
        self.write('README.md', 'Updated synthetic source\n')
        self.commit()
        self.git('push', '-q', 'origin', 'main')

    def invoke(self, notes='', dispatch_fails=False, **environment):
        real_run = subprocess.run
        calls = []

        def run(command, **kwargs):
            if command[0] == 'gh':
                calls.append(command)
                self.assertEqual(self.git('rev-parse', 'HEAD'), self.git('rev-parse', 'origin/main'))
                self.assertEqual(self.git('rev-parse', 'HEAD'), self.git('rev-parse', command[-1] + '^{commit}'))
                if dispatch_fails:
                    raise subprocess.CalledProcessError(1, command)
                return subprocess.CompletedProcess(command, 0)
            return real_run(command, **kwargs)

        env = dict(GITHUB_REF='refs/heads/main', GITHUB_EVENT_NAME='workflow_dispatch',
                   GITHUB_REPOSITORY='fixture/shin', GITHUB_SHA=self.git('rev-parse', 'HEAD'),
                   GITHUB_STEP_SUMMARY=str(self.root.parent / 'summary.md'), RELEASE_NOTES=notes)
        env.update(environment)
        with patch.dict(os.environ, env), patch.object(release, '__file__', str(self.root / 'tools/create_preview.py')), \
                patch.object(subprocess, 'run', side_effect=run):
            release.main()
        return calls

    def test_next_preview_preserves_core_policy_and_source(self):
        self.change()
        tag, paths = release.prepare(self.root, '- Focused fixture fix')
        self.assertEqual(tag, 'v7.8.9-dev.5')
        self.assertEqual(set(paths), {'observer/VERSION', release.POLICY, 'docs/releases/' + tag + '.md'})
        self.assertEqual((self.root / 'VERSION').read_text(), '7.0.0-dev.3\n')
        expected = json.loads(json.dumps(self.policy))
        expected['developmentLane']['observerSourceVersion'] = '7.8.9-dev.5'
        self.assertEqual(json.loads((self.root / release.POLICY).read_text()), expected)
        self.assertEqual((self.root / 'README.md').read_text(), 'Updated synthetic source\n')

    def test_prepared_version_and_notes_are_preserved(self):
        self.policy['developmentLane']['observerSourceVersion'] = '7.8.9-dev.5'
        self.write('observer/VERSION', '7.8.9-dev.5\n')
        self.write(release.POLICY, json.dumps(self.policy))
        self.write('docs/releases/v7.8.9-dev.5.md', '# v7.8.9-dev.5\n\nCurated fixture notes.\n')
        self.commit()
        tag, paths = release.prepare(self.root)
        self.assertEqual((tag, paths), ('v7.8.9-dev.5', []))
        self.assertIn('Curated fixture notes.', (self.root / ('docs/releases/' + tag + '.md')).read_text())
        with self.assertRaisesRegex(ValueError, 'Prepared notes already exist'):
            release.prepare(self.root, 'Do not overwrite curated notes')

    def test_default_notes_use_new_commit_subjects(self):
        self.change()
        tag, _ = release.prepare(self.root)
        notes = (self.root / ('docs/releases/' + tag + '.md')).read_text()
        self.assertIn('- Fixture change\n', notes)
        self.assertNotIn('- Previous preview\n', notes)

    def test_metadata_and_note_failures_do_not_write_versions(self):
        self.change()
        original = (self.root / 'observer/VERSION').read_text()
        for notes in ('x' * 8193, 'invalid\0notes'):
            with self.assertRaises(ValueError):
                release.prepare(self.root, notes)
            self.assertEqual((self.root / 'observer/VERSION').read_text(), original)
        self.policy['developmentLane']['coreVersion'] = '7.0.0-dev.2'
        self.write(release.POLICY, json.dumps(self.policy))
        with self.assertRaisesRegex(ValueError, 'Component versions differ'):
            release.prepare(self.root)
        self.assertEqual((self.root / 'observer/VERSION').read_text(), original)

    def test_existing_tag_is_never_moved_and_older_main_is_refused(self):
        self.change()
        self.git('tag', 'v7.8.9-dev.5')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            release.prepare(self.root)
        self.git('tag', 'v8.0.0-dev.0')
        with self.assertRaisesRegex(ValueError, 'behind a newer preview'):
            release.prepare(self.root)
        self.assertEqual((self.root / 'observer/VERSION').read_text(), '7.8.9-dev.4\n')

    def test_version_bound_and_symlink_notes_fail_before_writes(self):
        self.change()
        note = self.root / 'docs/releases/v7.8.9-dev.5.md'
        note.symlink_to(self.root / 'README.md')
        with self.assertRaisesRegex(ValueError, 'regular source file'):
            release.prepare(self.root)
        note.unlink()
        self.write('observer/VERSION', '7.8.9-dev.999999999\n')
        self.policy['developmentLane']['observerSourceVersion'] = '7.8.9-dev.999999999'
        self.write(release.POLICY, json.dumps(self.policy))
        self.commit()
        self.git('tag', 'v7.8.9-dev.999999999')
        self.write('README.md', 'Source after the upper-bound preview\n')
        self.commit()
        with self.assertRaisesRegex(ValueError, 'canonical preview version'):
            release.prepare(self.root)

    def test_real_git_commit_and_atomic_tag_push_precede_dispatch(self):
        self.change()
        calls = self.invoke('- Literal $(not-a-command) stays Markdown')
        self.assertEqual(calls, [['gh', 'workflow', 'run', 'ci.yml', '--repo', 'fixture/shin', '--ref', 'v7.8.9-dev.5']])
        self.assertEqual(self.git('status', '--porcelain'), '')
        self.assertIn('github-actions[bot]', self.git('show', '-s', '--format=%an'))
        self.assertIn('$(not-a-command)', (self.root / 'docs/releases/v7.8.9-dev.5.md').read_text())
        self.assertIn('Follow builds, tests and publication', (self.root.parent / 'summary.md').read_text())

    def test_failed_dispatch_resumes_same_tag_without_another_commit(self):
        self.change()
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(dispatch_fails=True)
        previous = self.git('rev-parse', 'HEAD')
        self.invoke()
        self.assertEqual(previous, self.git('rev-parse', 'HEAD'))
        self.assertEqual(self.git('tag', '--list', 'v7.8.9-dev.6'), '')

    def test_concurrent_main_push_leaves_no_release_tag_on_origin(self):
        self.change()
        writer = self.root.parent / 'concurrent-writer'
        subprocess.run(['git', 'clone', '-q', '--branch', 'main', str(self.origin), str(writer)], check=True)
        real_git = release.git

        def git(root, *args):
            if args[:2] == ('push', '--atomic'):
                (writer / 'concurrent.txt').write_text('Another maintainer changed main\n')
                real_git(writer, 'add', 'concurrent.txt')
                real_git(writer, *release.BOT, 'commit', '-m', 'Concurrent source change')
                real_git(writer, 'push', 'origin', 'main')
            return real_git(root, *args)

        with patch.object(release, 'git', side_effect=git), self.assertRaises(subprocess.CalledProcessError):
            self.invoke()
        remote_tag = subprocess.check_output(['git', '--git-dir', str(self.origin),
            'tag', '--list', 'v7.8.9-dev.5'], text=True).strip()
        self.assertEqual(remote_tag, '')

    def test_branch_dirty_checkout_and_moved_main_refuse_publication(self):
        with self.assertRaisesRegex(ValueError, 'from main'):
            self.invoke(GITHUB_REF='refs/heads/other')
        self.write('README.md', 'Uncommitted source\n')
        with self.assertRaisesRegex(ValueError, 'not clean'):
            self.invoke()
        self.commit()
        with self.assertRaisesRegex(ValueError, 'main moved'):
            self.invoke()
        self.assertEqual(self.git('tag', '--list', 'v7.8.9-dev.5'), '')


if __name__ == '__main__':
    unittest.main()
