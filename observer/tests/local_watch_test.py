#!/usr/bin/env python3
"""Continuous native reconciliation; no shell indexer, Server or Git required."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest

import local_project_test as fixtures


class LocalWatchTests(unittest.TestCase):
    setUp = fixtures.LocalProjectTests.setUp
    run_ce = fixtures.LocalProjectTests.run_ce
    init = fixtures.LocalProjectTests.init
    workspace = fixtures.LocalProjectTests.workspace
    file_map = fixtures.LocalProjectTests.file_map
    write_private = fixtures.LocalProjectTests.write_private

    def start(self, root=None):
        child = subprocess.Popen([str(fixtures.TOOL), 'watch', '--root', str(root or self.root), '--home', str(self.home)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        def cleanup():
            if child.poll() is None:
                child.terminate()
            try:
                child.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                self.fail('local watcher did not stop')
        self.addCleanup(cleanup)
        return child

    def wait_state(self, condition, child, seconds=15):
        deadline = time.monotonic() + seconds
        path = self.workspace() / 'watch/state.json'
        state = None
        while time.monotonic() < deadline:
            self.assertIsNone(child.poll(), 'watcher exited unexpectedly')
            if path.exists():
                state = json.loads(path.read_text())
                if condition(state):
                    return state
            time.sleep(0.05)
        self.fail('watcher condition timed out: ' + str(state))

    def test_idle_checks_back_off_and_native_edits_wake_refresh(self):
        self.init()
        child = self.start()
        idle = self.wait_state(lambda s: s['phase'] == 'current' and s.get('retry_after_ms', 0) >= 8000, child, seconds=25)
        self.assertTrue(idle['change_monitor_healthy'])
        start = time.monotonic()
        (self.root / 'limits.js').write_text('export function readLimit() { return 901; }\n')
        current = self.wait_state(lambda s: s['phase'] == 'current' and s['publications'] == 1, child, seconds=8)
        self.assertLess(time.monotonic() - start, 8)
        self.assertEqual(current['idle_checks'], 0)
        self.assertEqual(current['retry_after_ms'], 1000)
        self.assertIn('return 901', self.run_ce('query', 'readLimit')['source_context'])

    def test_lost_monitor_rearms_only_for_original_root_then_requalifies(self):
        self.init()
        child = self.start()
        self.wait_state(lambda s: s['phase'] == 'current' and s.get('retry_after_ms', 0) >= 8000, child, seconds=25)
        moved = self.base / 'temporarily-moved'
        self.root.rename(moved)
        time.sleep(.2)  # Native root-loss event, while no refresh is due yet.
        moved.rename(self.root)
        (self.root / 'limits.js').write_text('export function readLimit() { return 902; }\n')
        recovered = self.wait_state(lambda s: s['phase'] == 'current' and s.get('change_monitor_recoveries', 0) > 0,
                                    child, seconds=45)
        self.assertTrue(recovered['change_monitor_healthy'])
        self.assertGreaterEqual(recovered['change_monitor_restarts'], 1)
        self.assertTrue(recovered['last_change_monitor_error'])
        self.assertIn('return 902', self.run_ce('query', 'readLimit')['source_context'])
        self.wait_state(lambda s: s['phase'] == 'current' and s.get('retry_after_ms', 0) >= 8000, child, seconds=25)
        self.root.rename(moved)
        self.root.mkdir()
        (self.root / 'limits.js').write_text('export function readLimit() { return 999; }\n')
        blocked = self.wait_state(lambda s: s['phase'] == 'blocked', child)
        self.assertEqual(blocked['change_monitor_restarts'], recovered['change_monitor_restarts'])
        self.assertFalse(blocked['last_result']['ok'])

    def test_dependency_noise_does_not_reset_idle_schedule(self):
        self.init()
        deps = self.root / 'node_modules'
        deps.mkdir()
        child = self.start()
        idle = self.wait_state(lambda s: s['phase'] == 'current' and s.get('retry_after_ms', 0) == 15000,
                               child, seconds=30)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            (deps / 'generated.js').write_text(str(time.monotonic()))
            time.sleep(.04)
        current = json.loads((self.workspace() / 'watch/state.json').read_text())
        self.assertEqual(current['ticks'], idle['ticks'])
        self.assertTrue(current['change_monitor_healthy'])

    def test_requires_initialized_binding_and_rejects_internal_command(self):
        self.assertEqual(self.run_ce('watch', '--samples', '1', expected=2)['error']['code'], 'refresh-binding-required')
        self.assertFalse(self.home.exists())
        for arguments in [['_refresh'], ['watch', '--samples', '0'], ['watch', '--samples', 'abc'], ['watch', '--samples', '3601']]:
            self.assertEqual(self.run_ce(*arguments, expected=2)['error']['code'], 'invalid-arguments')
        self.assertFalse(self.home.exists())

    def test_continuous_edits_add_rename_delete_and_idle_reads(self):
        self.init()
        child = self.start()
        self.wait_state(lambda s: s['phase'] == 'current', child)
        self.assertTrue(self.run_ce('status')['watch']['running'])
        self.assertIn('return 7', self.run_ce('query', 'readLimit')['source_context'])
        for value in [10, 11, 12]:
            (self.root / 'limits.js').write_text(f'export function readLimit() {{ return {value}; }}\n')
        added = self.root / 'added.js'
        added.write_text('export function addedNow() { return 19; }\n')
        self.wait_state(lambda s: s['phase'] == 'current' and s['publications'] == 1, child)
        self.assertIn('return 12', self.run_ce('query', 'readLimit')['source_context'])
        self.assertIn('addedNow', self.run_ce('query', 'addedNow')['source_context'])
        added.rename(self.root / 'moved.js')
        self.wait_state(lambda s: s['phase'] == 'current' and s['publications'] == 2, child)
        self.assertIn('moved.js', self.run_ce('query', 'addedNow')['source_context'])
        (self.root / 'moved.js').unlink()
        self.wait_state(lambda s: s['phase'] == 'current' and s['publications'] == 3, child)
        self.assertNotIn('addedNow', self.run_ce('query', 'addedNow')['source_context'])
        self.assertFalse((self.root / '.git').exists())
        child.terminate()
        stdout, stderr = child.communicate(timeout=20)
        self.assertEqual(child.returncode, 0, stderr)
        self.assertEqual(json.loads(stdout)['watch']['phase'], 'stopped')
        self.assertFalse(self.run_ce('status')['watch']['running'])

    def test_failed_candidate_preserves_previous_publication_and_binding(self):
        registry = self.base / 'profiles'
        registry.mkdir()
        profile = json.loads((fixtures.REGISTRY / 'generic-source.json').read_text())
        profile['sourceFactMaxFileBytes'] = 2 * 1024 * 1024
        (registry / 'generic-source.json').write_text(json.dumps(profile))
        self.init(profile_dir=registry)
        index = self.workspace() / 'index'
        before = self.file_map(index)
        binding = (self.workspace() / 'state.json').read_bytes()
        (self.root / 'large.js').write_text(' ' * (1024 * 1024 + 1))
        result = self.run_ce('watch', '--samples', '3')
        self.assertGreaterEqual(result['watch']['failures'], 1)
        self.assertEqual(result['watch']['publications'], 0)
        self.assertEqual(self.file_map(index), before)
        self.assertEqual((self.workspace() / 'state.json').read_bytes(), binding)
        self.assertFalse(list(self.workspace().glob('refresh-*')))
        query = self.run_ce('query', 'readLimit', expected=1)
        self.assertNotIn('source_context', query)

    def test_interrupted_controller_restarts_and_refreshes(self):
        self.init()
        child = self.start()
        self.wait_state(lambda s: s['phase'] == 'current', child)
        child.kill()
        child.communicate(timeout=10)
        (self.root / 'limits.js').write_text('export function readLimit() { return 23; }\n')
        result = self.run_ce('watch', '--samples', '3')
        self.assertTrue(result['watch']['previous_run_interrupted'])
        self.assertEqual(result['watch']['publications'], 1)
        self.assertIn('return 23', self.run_ce('query', 'readLimit')['source_context'])

    def test_corrupt_controller_metadata_is_recreated_without_init(self):
        self.init()
        self.run_ce('watch', '--samples', '1')
        state_path = self.workspace() / 'watch/state.json'
        binding = (self.workspace() / 'state.json').read_bytes()
        before = self.file_map(self.workspace() / 'index')
        for content in ['{', '{}', '[]', '{"phase":false}']:
            with self.subTest(content=content):
                state_path.write_text(content)
                result = self.run_ce('watch', '--samples', '1')['watch']
                self.assertEqual(result['failures'], 0)
                self.assertEqual(result['recoveries'], 1)
                self.assertEqual(result['last_recovery'], 'watch-state')
                self.assertTrue(self.run_ce('status')['local_ready'])
                self.assertEqual(binding, (self.workspace() / 'state.json').read_bytes())
                self.assertEqual(before, self.file_map(self.workspace() / 'index'))

    def test_private_bootstrap_repairs_generated_payload_without_init(self):
        # Product-owned output remains private even from a permissive shell.
        previous = os.umask(0)
        try:
            self.init()
        finally:
            os.umask(previous)
        target = self.workspace() / 'index/docs/kb/generated/documents.jsonl'
        self.assertEqual(target.stat().st_mode & 0o077, 0)
        self.assertEqual(target.parent.stat().st_mode & 0o077, 0)
        target.write_bytes(b'corrupt payload')
        result = self.run_ce('watch', '--samples', '2')['watch']
        self.assertEqual(result['failures'], 0)
        self.assertEqual(result['recoveries'], 1)
        self.assertEqual(result['last_recovery'], 'primary-index')
        self.assertIn('return 7', self.run_ce('query', 'readLimit')['source_context'])

    def test_controller_metadata_recovery_refuses_foreign_root_and_symlink(self):
        self.init()
        self.run_ce('watch', '--samples', '1')
        state_path = self.workspace() / 'watch/state.json'
        value = json.loads(state_path.read_text())
        value['root'] = str(self.base / 'other')
        self.write_private(state_path, value)
        self.assertEqual(self.run_ce('watch', '--samples', '1', expected=2)['error']['code'], 'watch-state-invalid')
        target = self.base / 'unowned.json'
        state_path.rename(target)
        state_path.symlink_to(target)
        self.run_ce('watch', '--samples', '1', expected=2)
        self.assertTrue(state_path.is_symlink())
        self.assertEqual(json.loads(target.read_text()), value)

    def test_duplicate_controller_and_later_adoption_fail_closed(self):
        self.init()
        child = self.start()
        self.wait_state(lambda s: s['phase'] == 'current', child)
        self.assertEqual(self.run_ce('watch', '--samples', '1', expected=2)['error']['code'], 'writer-busy')
        before = self.file_map(self.workspace() / 'index')
        marker = self.root / 'docs/machine/shin-project-adoption-v1.json'
        marker.parent.mkdir(parents=True)
        marker.write_text('{}')
        state = self.wait_state(lambda s: s['phase'] == 'blocked', child)
        self.assertEqual(state['last_result']['error']['code'], 'existing-project-adoption')
        self.assertEqual(before, self.file_map(self.workspace() / 'index'))

    def test_recovers_prepared_receipt_gap_without_republishing(self):
        self.init()
        state_path = self.workspace() / 'state.json'
        old = json.loads(state_path.read_text())
        (self.root / 'limits.js').write_text('export function readLimit() { return 31; }\n')
        self.init()
        before = self.file_map(self.workspace() / 'index')
        current = json.loads(state_path.read_text())
        current['prepared_snapshot'] = old['prepared_snapshot']
        self.write_private(state_path, current)
        self.assertFalse(self.run_ce('status', expected=1)['local_ready'])
        result = self.run_ce('watch', '--samples', '3')
        self.assertEqual(result['watch']['publications'], 0)
        self.assertEqual(before, self.file_map(self.workspace() / 'index'))
        self.assertIn('return 31', self.run_ce('query', 'readLimit')['source_context'])

    def test_retargeted_alias_cannot_switch_watched_project(self):
        self.init()
        other = self.base / 'other'
        other.mkdir()
        (other / 'README.md').write_text('# Other project\n')
        (other / 'other.js').write_text('export function otherValue() { return 1; }\n')
        initialized = self.init(root=other)
        before = self.file_map(Path(initialized['index']))
        alias = self.base / 'alias'
        alias.symlink_to(self.root, target_is_directory=True)
        child = self.start(alias)
        self.wait_state(lambda s: s['phase'] == 'current', child)
        alias.unlink()
        alias.symlink_to(other, target_is_directory=True)
        (self.root / 'limits.js').write_text('export function readLimit() { return 37; }\n')
        (other / 'other.js').write_text('export function otherValue() { return 2; }\n')
        self.wait_state(lambda s: s['phase'] == 'current' and s['publications'] == 1, child)
        self.assertIn('return 37', self.run_ce('query', 'readLimit')['source_context'])
        self.assertFalse(self.run_ce('status', root=other, expected=1)['local_ready'])
        self.assertEqual(before, self.file_map(Path(initialized['index'])))


if __name__ == '__main__':
    unittest.main()
