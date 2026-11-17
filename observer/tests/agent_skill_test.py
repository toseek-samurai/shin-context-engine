#!/usr/bin/env python3
"""Native instruction ownership, reviewed updates and interruption recovery.

Uses a disposable package and Codex home; never reads host agent settings.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

binary = Path(sys.argv.pop(1)).resolve()
os.umask(0o077)


def checksum(text):
    return hashlib.sha256(text.encode()).hexdigest()


class AgentSkillTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='shin-agent-skill-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        if os.name == 'nt':
            from windows_shell_qualification import private
            identity = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'],
                                               encoding='utf-8', errors='replace')
            self.sid = re.search(r'S-1-5-[0-9-]+', identity).group()
            private(self.base, self.sid, directory=True)
        self.bin = self.base / 'kit/bin' / binary.name
        self.bin.parent.mkdir(parents=True)
        shutil.copy2(binary, self.bin)
        self.resources = self.base / 'kit/share/shin-observer/agent-skills/codex'
        self.config = self.base / 'codex/config.toml'
        self.target = self.config.parent / 'skills/shin-context'
        self.state = self.config.parent / '.shin-agent-skills/shin-context/state.json'
        self.files = {'SKILL.md': '# Shin\n\nSee [guide](references/guide.md).\n',
                      'references/guide.md': '# Read safely\n\nKeep qualification.\n'}
        self.bundle(self.files)

    def bundle(self, files, version='2.0.5-dev.999'):
        if self.resources.exists():
            shutil.rmtree(self.resources)
        for name, text in files.items():
            path = self.resources / 'shin-context' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8', newline='\n')
        self.resources.mkdir(parents=True, exist_ok=True)
        (self.resources / 'bundle.json').write_text(json.dumps({
            'schema_version': 'shin.codex-skill-bundle.v1', 'name': 'shin-context',
            'version': version, 'files': {name: checksum(text) for name, text in files.items()}}), encoding='utf-8', newline='\n')

    def run_cli(self, *args, ok=True):
        result = subprocess.run([str(self.bin), 'codex-skill', *map(str, args)],
            env={**os.environ, 'CODEX_HOME': str(self.config.parent)},
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode == 0, ok, (result.stdout, result.stderr))
        return json.loads(result.stdout or result.stderr)

    def preview(self):
        return self.run_cli('--config', self.config)

    def apply(self, plan, ok=True):
        return self.run_cli('--config', self.config, '--revision', plan['revision'], ok=ok)

    def private_file(self, path):
        if os.name == 'nt':
            from windows_shell_qualification import private
            # An elevated Windows token can give Python-created directories the
            # Administrators owner even beneath a user-owned temporary root.
            # Model an existing user-owned skill, including its parent chain.
            self.assertIn(self.base, path.parents)
            parents = []
            directory = path.parent
            while directory != self.base:
                parents.append(directory)
                directory = directory.parent
            for directory in reversed(parents):
                private(directory, self.sid, directory=True)
            private(path, self.sid)

    def install(self):
        self.apply(self.preview())

    def test_preview_does_not_create_files_and_default_codex_home_is_used(self):
        plan = self.run_cli()
        self.assertEqual(plan['state'], 'not-installed')
        self.assertTrue(plan['can_apply'])
        self.assertFalse(plan['client_modified'])
        self.assertFalse(self.config.parent.exists())

    def test_install_idempotence_and_settings_preservation(self):
        self.config.parent.mkdir()
        self.config.write_text('# user settings\nmodel="example"\n', encoding='utf-8', newline='\n')
        result = self.apply(self.preview())
        self.assertTrue(result['client_modified'])
        self.assertTrue(result['managed'])
        for name, text in self.files.items():
            self.assertEqual((self.target / name).read_text(), text)
        self.assertEqual(self.config.read_text(), '# user settings\nmodel="example"\n')
        self.assertEqual(self.preview()['state'], 'current')
        self.assertFalse(self.apply(self.preview())['client_modified'])

    def test_managed_update_addition_removal_and_unknown_file_preservation(self):
        self.install()
        extra = self.target / 'my-notes.md'
        extra.write_text('# Mine\n', encoding='utf-8', newline='\n')
        updated = {'SKILL.md': '# New instructions\n', 'references/new.md': '# New guide\n'}
        self.bundle(updated)
        plan = self.preview()
        self.assertEqual(plan['state'], 'update-available')
        self.apply(plan)
        self.assertFalse((self.target / 'references/guide.md').exists())
        for name, text in updated.items():
            self.assertEqual((self.target / name).read_text(), text)
        self.assertEqual(extra.read_text(), '# Mine\n')
        self.assertEqual(self.preview()['state'], 'current')

    def test_customized_owned_file_is_never_overwritten(self):
        self.install()
        entry = self.target / 'SKILL.md'
        entry.write_text('# My own instructions\n', encoding='utf-8', newline='\n')
        self.bundle({**self.files, 'SKILL.md': '# Upstream\n'})
        plan = self.preview()
        self.assertEqual(plan['state'], 'customized')
        self.assertFalse(plan['can_apply'])
        self.assertEqual(plan['modified_files'], ['SKILL.md'])
        self.assertEqual(self.apply(plan, ok=False)['error']['code'], 'agent-skill-customized')
        self.assertEqual(entry.read_text(), '# My own instructions\n')

    def test_deleted_owned_page_is_preserved_as_a_user_change(self):
        self.install()
        (self.target / 'references/guide.md').unlink()
        plan = self.preview()
        self.assertEqual(plan['state'], 'customized')
        self.assertFalse(plan['can_apply'])
        self.assertFalse((self.target / 'references/guide.md').exists())

    def test_exact_unmanaged_copy_can_be_adopted_without_rewriting(self):
        for name, text in self.files.items():
            file = self.target / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(text, encoding='utf-8', newline='\n')
            self.private_file(file)
        plan = self.preview()
        self.assertEqual(plan['state'], 'current')
        self.assertFalse(plan['managed'])
        inode = (self.target / 'SKILL.md').stat().st_ino
        result = self.apply(plan)
        self.assertFalse(result['client_modified'])
        self.assertTrue(self.preview()['managed'])
        self.assertEqual((self.target / 'SKILL.md').stat().st_ino, inode)

    def test_different_unmanaged_copy_needs_manual_review(self):
        self.target.mkdir(parents=True)
        (self.target / 'SKILL.md').write_text('# Unknown old or custom copy\n', encoding='utf-8', newline='\n')
        self.private_file(self.target / 'SKILL.md')
        plan = self.preview()
        self.assertEqual(plan['state'], 'customized')
        self.apply(plan, ok=False)
        self.assertFalse(self.state.exists())

    def test_stale_review_refuses_new_content_and_new_bundle(self):
        plan = self.preview()
        self.bundle({**self.files, 'references/guide.md': '# Changed upstream\n'})
        self.assertEqual(self.apply(plan, ok=False)['error']['code'], 'agent-skill-changed')
        self.assertFalse(self.target.exists())
        self.install()
        plan = self.preview()
        (self.target / 'SKILL.md').write_text('# User edit\n', encoding='utf-8', newline='\n')
        self.assertEqual(self.apply(plan, ok=False)['error']['code'], 'agent-skill-changed')

    def test_linked_target_and_file_are_refused(self):
        elsewhere = self.base / 'elsewhere'
        elsewhere.mkdir()
        self.target.parent.mkdir(parents=True)
        if os.name == 'nt':
            # Junctions and hard links need no administrator/symlink privilege.
            linked = subprocess.run([os.environ['COMSPEC'], '/d', '/c', 'mklink', '/J',
                                     str(self.target), str(elsewhere)], capture_output=True,
                                    encoding='utf-8', errors='replace')
            self.assertEqual(linked.returncode, 0, (linked.stdout, linked.stderr))
        else:
            self.target.symlink_to(elsewhere, target_is_directory=True)
        self.run_cli('--config', self.config, ok=False)
        if os.name == 'nt':
            self.target.rmdir()
        else:
            self.target.unlink()
        self.install()
        page = self.target / 'references/guide.md'
        page.unlink()
        if os.name == 'nt':
            os.link(self.target / 'SKILL.md', page)
        else:
            page.symlink_to(self.target / 'SKILL.md')
        self.run_cli('--config', self.config, ok=False)

    def test_corrupt_bundle_is_refused_without_writes(self):
        (self.resources / 'shin-context/SKILL.md').write_text('# Tampered\n', encoding='utf-8', newline='\n')
        self.assertEqual(self.run_cli('--config', self.config, ok=False)['error']['code'], 'agent-skill-bundle-invalid')
        self.assertFalse(self.config.parent.exists())

    def interrupt(self):
        self.install()
        receipt = json.loads(self.state.read_text())
        before = self.files['SKILL.md']
        after = '# Interrupted upstream\n'
        (self.target / 'SKILL.md').write_text(after, encoding='utf-8', newline='\n')
        receipt['pending'] = {'directories': {'': receipt['directory_identity']},
            'edits': [{'path': 'SKILL.md', 'before': before, 'after': checksum(after)}]}
        self.state.write_text(json.dumps(receipt), encoding='utf-8', newline='\n')
        return before, after

    def test_interruption_is_reviewed_and_rolled_back_without_replay(self):
        before, after = self.interrupt()
        # Even another current bundle does not replay the abandoned update.
        self.bundle({**self.files, 'SKILL.md': '# Newer unrelated update\n'})
        plan = self.preview()
        self.assertEqual(plan['state'], 'recovery-required')
        self.assertEqual((self.target / 'SKILL.md').read_text(), after)
        result = self.apply(plan)
        self.assertTrue(result['recovered'])
        self.assertEqual((self.target / 'SKILL.md').read_text(), before)
        self.assertEqual(self.preview()['state'], 'update-available')

    def test_interrupted_update_preserves_intervening_user_edit(self):
        self.interrupt()
        entry = self.target / 'SKILL.md'
        entry.write_text('# Written by the user after interruption\n', encoding='utf-8', newline='\n')
        plan = self.preview()
        self.assertFalse(plan['can_apply'])
        self.apply(plan, ok=False)
        self.assertEqual(entry.read_text(), '# Written by the user after interruption\n')
        self.assertIn('pending', json.loads(self.state.read_text()))

    def test_inventory_path_traversal_is_refused(self):
        descriptor = self.resources / 'bundle.json'
        value = json.loads(descriptor.read_text())
        value['files']['../escape.md'] = checksum('escape')
        descriptor.write_text(json.dumps(value), encoding='utf-8', newline='\n')
        self.run_cli('--config', self.config, ok=False)
        self.assertFalse(self.config.parent.exists())


if __name__ == '__main__':
    unittest.main()
