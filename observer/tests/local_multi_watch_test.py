#!/usr/bin/env python3
"""Five-project scheduling and query isolation using the native local controller."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import unittest

import local_project_test as fixtures


class MultiWatchTests(unittest.TestCase):
    setUp = fixtures.LocalProjectTests.setUp
    run_ce = fixtures.LocalProjectTests.run_ce
    init = fixtures.LocalProjectTests.init
    file_map = fixtures.LocalProjectTests.file_map

    def projects(self, count=5):
        roots = [self.root]
        for index in range(1, count):
            root = self.base / f'project-{index}'
            root.mkdir()
            (root / 'README.md').write_text(f'# Project {index}\n\nreadLimit owns this project limit.\n')
            roots.append(root)
        for index, root in enumerate(roots):
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {100 + index}; }}\n')
            self.init(root=root)
        return roots

    def workspace(self, root):
        return self.home / 'projects' / hashlib.sha256(str(root).encode()).hexdigest()

    def command(self, roots, *extra):
        args = [str(fixtures.TOOL), 'watch', '--home', str(self.home), *extra]
        for root in roots:
            args += ['--root', str(root)]
        return args

    def start(self, roots):
        child = subprocess.Popen(self.command(roots), text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        def cleanup():
            if child.poll() is None:
                child.terminate()
            try:
                child.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                self.fail('controller failed cooperative stop')
        self.addCleanup(cleanup)
        return child

    def wait(self, roots, child, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        states = []
        while time.monotonic() < deadline:
            self.assertIsNone(child.poll(), 'controller exited unexpectedly')
            states = []
            for root in roots:
                path = self.workspace(root) / 'watch/state.json'
                states.append(json.loads(path.read_text()) if path.exists() else {})
            if all(predicate(state) for state in states):
                return states
            time.sleep(0.05)
        self.fail('controller progress timed out: ' + str(states))

    def query_all(self, roots, first_value):
        def ready_query(root):
            # Independent one-shot readers may meet the next background writer.
            # Retry only the declared transient refusal, never stale/failed data.
            deadline = time.monotonic() + 5
            while True:
                result = subprocess.run([str(fixtures.TOOL), 'query', 'readLimit', '--root', str(root),
                                         '--home', str(self.home)], capture_output=True, text=True, timeout=15,
                                        env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
                value = json.loads(result.stdout or result.stderr)
                if result.returncode == 2 and value.get('error', {}).get('code') == 'project-busy' and time.monotonic() < deadline:
                    time.sleep(0.02)
                    continue
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(result.stderr)
                self.assertLessEqual(len(result.stdout.encode()), 65536)
                return value
        with ThreadPoolExecutor(max_workers=len(roots)) as pool:
            queries = list(pool.map(ready_query, roots))
        self.assertEqual(len({q['project_id'] for q in queries}), len(roots))
        self.assertEqual(len({q['workspace_id'] for q in queries}), len(roots))
        self.assertEqual(len({q['snapshot_id'] for q in queries}), len(roots))
        for index, result in enumerate(queries):
            self.assertEqual(result['root'], str(roots[index]))
            self.assertIn(f'return {first_value + index};', result['source_context'])
            for other in range(len(roots)):
                if other != index:
                    self.assertNotIn(f'return {first_value + other};', result['source_context'])
        return queries

    def test_five_projects_refresh_query_followup_and_restart(self):
        roots = self.projects()
        child = self.start(roots)
        self.wait(roots, child, lambda s: s.get('phase') == 'current')
        self.query_all(roots, 100)
        for index, root in enumerate(roots):
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {200 + index}; }}\n')
        self.wait(roots, child, lambda s: s.get('phase') == 'current' and s['publications'] == 1)
        queries = self.query_all(roots, 200)
        wrong = self.run_ce('read', 'limits.js', '--snapshot', queries[0]['snapshot_id'], root=roots[1], expected=2)
        self.assertNotIn('source_context', wrong)
        self.assertFalse(wrong['ok'])
        child.kill()
        child.communicate(timeout=10)
        for index, root in enumerate(roots):
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {300 + index}; }}\n')
        child = self.start(roots)
        states = self.wait(roots, child, lambda s: s.get('phase') == 'current' and
                           s.get('previous_run_interrupted') and s['publications'] == 1)
        self.assertEqual(len(states), 5)
        self.query_all(roots, 300)
        child.terminate()
        stdout, stderr = child.communicate(timeout=20)
        self.assertEqual(child.returncode, 0, stderr)
        result = json.loads(stdout)
        self.assertEqual(result['scheduler']['max_active_refreshes'], 1)
        self.assertEqual(len(result['projects']), 5)
        self.assertTrue(all(p['phase'] == 'stopped' for p in result['projects']))

    def test_one_blocked_project_does_not_block_four_healthy_projects(self):
        roots = self.projects()
        child = self.start(roots)
        self.wait(roots, child, lambda s: s.get('phase') == 'current')
        before = self.file_map(self.workspace(roots[0]) / 'index')
        marker = roots[0] / 'docs/machine/shin-project-adoption-v1.json'
        marker.parent.mkdir(parents=True)
        marker.write_text('{}')
        for index, root in enumerate(roots):
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {400 + index}; }}\n')
        self.wait(roots[:1], child, lambda s: s.get('phase') == 'blocked')
        self.wait(roots[1:], child, lambda s: s.get('phase') == 'current' and s['publications'] == 1)
        self.query_all(roots[1:], 401)
        self.assertEqual(before, self.file_map(self.workspace(roots[0]) / 'index'))
        marker.unlink()
        self.wait(roots[:1], child, lambda s: s.get('phase') == 'current' and s['publications'] == 1)
        self.assertIn('return 400;', self.run_ce('query', 'readLimit')['source_context'])

    def test_replaced_controller_storage_is_isolated(self):
        roots = self.projects(2)
        child = self.start(roots)
        self.wait(roots, child, lambda s: s.get('phase') == 'current')
        directory = self.workspace(roots[0]) / 'watch'
        directory.rename(directory.with_name('watch-preserved'))
        directory.mkdir(mode=0o700)
        (roots[1] / 'limits.js').write_text('export function readLimit() { return 501; }\n')
        self.wait(roots[1:], child, lambda s: s.get('phase') == 'current' and s['publications'] == 1)
        self.assertIn('return 501;', self.run_ce('query', 'readLimit', root=roots[1])['source_context'])
        self.assertEqual(list(directory.iterdir()), [])
        child.terminate()
        stdout, stderr = child.communicate(timeout=20)
        self.assertEqual(child.returncode, 0, stderr)
        failed = json.loads(stdout)['projects'][0]
        self.assertEqual(failed['phase'], 'blocked')
        self.assertFalse(failed['state_persisted'])
        self.assertEqual(failed['last_result']['error']['code'], 'watch-state-unavailable')

    def test_overlap_with_running_controller_releases_partial_admission(self):
        roots = self.projects(2)
        child = self.start(roots[1:])
        self.wait(roots[1:], child, lambda s: s.get('phase') == 'current')
        before = self.file_map(self.workspace(roots[0]) / 'index')
        (roots[0] / 'limits.js').write_text('export function readLimit() { return 601; }\n')
        run = subprocess.run(self.command(roots, '--samples', '3'), capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertIn(json.loads(run.stderr)['error']['code'], ('writer-busy',))
        self.assertEqual(before, self.file_map(self.workspace(roots[0]) / 'index'))
        run = subprocess.run(self.command(roots[:1], '--samples', '3'), capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(json.loads(run.stdout)['watch']['publications'], 1)
        self.assertIsNone(child.poll())
        self.assertIn('return 101;', self.run_ce('query', 'readLimit', root=roots[1])['source_context'])

    def test_conflicts_admission_failure_and_sample_counts(self):
        roots = self.projects(2)
        alias = self.base / 'alias'
        alias.symlink_to(roots[0], target_is_directory=True)
        for selected, code in [([roots[0], alias], 'watch-root-conflict'),
                               ([roots[0], roots[0] / 'sub'], 'watch-root-conflict'),
                               ([roots[0]] * 17, 'invalid-arguments')]:
            (roots[0] / 'sub').mkdir(exist_ok=True)
            run = subprocess.run(self.command(selected, '--samples', '1'), capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
            self.assertEqual(json.loads(run.stderr)['error']['code'], code)
            self.assertFalse((self.workspace(roots[0]) / 'watch').exists())
        missing = self.base / 'uninitialized'
        missing.mkdir()
        before = self.file_map(self.workspace(roots[0]) / 'index')
        (roots[0] / 'limits.js').write_text('export function readLimit() { return 999; }\n')
        run = subprocess.run(self.command([roots[0], missing], '--samples', '3'), capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
        self.assertEqual(json.loads(run.stderr)['error']['code'], 'refresh-binding-required')
        self.assertEqual(before, self.file_map(self.workspace(roots[0]) / 'index'))
        run = subprocess.run(self.command(roots, '--samples', '3'), capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual([p['ticks'] for p in json.loads(run.stdout)['projects']], [3, 3])


if __name__ == '__main__':
    unittest.main()
