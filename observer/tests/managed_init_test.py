#!/usr/bin/env python3
"""Managed native init without a shell, global installation or a server."""
from concurrent.futures import ThreadPoolExecutor
import json
import hashlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

import local_project_test as fixtures


class ManagedInitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-init-', dir='/tmp')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.project('project')
        self.home = self.base / 'home'
        self.endpoint = self.home / 'service/rpc.sock'
        self.registry = self.home / 'registry/state.json'
        self.addCleanup(self.stop)

    def project(self, name, value=7):
        root = self.base / name
        root.mkdir()
        (root / 'limits.js').write_text(f'export function readLimit() {{ return {value}; }}\n')
        (root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
        return root

    def run_ce(self, command, *args, root=None, expected=0):
        run = subprocess.run([str(fixtures.TOOL), command, *args, '--root', str(root or self.root),
                              '--home', str(self.home)], capture_output=True, text=True, timeout=45,
                             env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        diagnostic = json.loads(run.stdout or run.stderr)
        diagnostic.pop('readiness', None)
        self.assertEqual(run.returncode, expected, json.dumps(diagnostic))
        self.assertLessEqual(len(run.stdout.encode()), 65536)
        if expected == 2:
            self.assertFalse(run.stdout)
            return json.loads(run.stderr)
        self.assertFalse(run.stderr)
        return json.loads(run.stdout)

    def stop(self):
        if not self.endpoint.exists() or not self.endpoint.is_socket():
            return
        self.run_ce('service-stop')
        deadline = time.monotonic() + 10
        while self.endpoint.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(self.endpoint.exists(), 'managed service did not stop')

    def await_status(self, predicate):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            status = self.run_ce('service-status')
            if predicate(status):
                return status
            time.sleep(0.05)
        self.fail('service status did not converge: ' + json.dumps(status))

    def test_default_init_five_projects_same_instance_and_refresh(self):
        first = self.run_ce('init')
        self.assertTrue(first['daemon_started'])
        self.assertTrue(first['service_enrolled'])
        self.assertTrue(first['local_ready'])
        self.assertFalse(first['canonical_ready'])
        self.assertEqual(first['service']['enrollment_mode'], 'registry')
        self.assertNotIn('telemetry', first['service'], 'init must not embed service history')
        instance = first['service']['instance_id']
        self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.registry.stat().st_mode & 0o777, 0o600)
        roots = [self.root] + [self.project(f'project-{i}', 7 + i) for i in range(1, 5)]
        for index, root in enumerate(roots):
            result = self.run_ce('init', root=root)
            self.assertFalse(result['daemon_started'])
            self.assertTrue(result['service_enrolled'])
            self.assertEqual(result['service']['instance_id'], instance)
            self.assertNotIn('telemetry', result['service'])
            self.assertIn(f'return {7 + index};', self.run_ce('query', 'readLimit', '--service', root=root)['source_context'])
        self.assertEqual(len(json.loads(self.registry.read_text())['projects']), 5)
        sixth = self.run_ce('init', root=self.project('sixth'))
        self.assertTrue(sixth['service_enrolled'])
        self.assertEqual(len(json.loads(self.registry.read_text())['projects']), 6)
        self.assertEqual(self.run_ce('service-status')['instance_id'], instance)
        warmed = self.run_ce('query', 'readLimit')
        self.assertIn('return 7;', warmed['source_context'])
        self.assertEqual(warmed['work_performed']['full_generations'], 0)
        (self.root / 'limits.js').write_text('export function readLimit() { return 77; }\n')
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            status = self.run_ce('service-status')
            self.assertEqual(status['instance_id'], instance)
            run = subprocess.run([str(fixtures.TOOL), 'query', 'readLimit', '--service', '--root', str(self.root),
                                  '--home', str(self.home)], capture_output=True, text=True, timeout=40)
            if run.returncode == 0 and 'return 77;' in json.loads(run.stdout)['source_context']:
                break
            time.sleep(0.1)
        else:
            self.fail('managed service did not refresh the changed source')

    def test_registered_catalog_exceeds_hot_pool_without_churn_or_stale_reads(self):
        roots = [self.root] + [self.project(f'idle-{i}', 100 + i) for i in range(1, 16)]
        instance = None
        for root in roots:
            result = self.run_ce('init', root=root)
            instance = instance or result['service']['instance_id']
            self.assertEqual(result['service']['instance_id'], instance)
            self.assertLessEqual(len(result['service']['resident_projects']), 5)
        status = self.run_ce('service-status')
        self.assertEqual(len(status['projects']), 16)
        self.assertEqual(len(status['qualified_projects']), 16)
        self.assertEqual(status['max_registered_projects'], 16)
        self.assertEqual(status['max_resident_projects'], 5)
        self.assertFalse(status['resident_errors'])
        self.assertTrue(any(row['query_phase'] == 'standby' for row in status['telemetry']['project_states']))
        registry = self.registry.read_bytes()
        overflow = self.run_ce('init', root=self.project('overflow'), expected=1)
        self.assertEqual(overflow['service']['error']['code'], 'registry-capacity')
        self.assertEqual(self.registry.read_bytes(), registry)
        evictions = status['resident_evictions']
        self.await_status(lambda current: all(row.get('ticks', 0) >= 2 for row in current['telemetry']['project_states']))
        self.assertEqual(self.run_ce('service-status')['resident_evictions'], evictions,
                         'background refresh must not churn the hot pool')
        cold = self.run_ce('query', 'readLimit', root=roots[0])
        self.assertIn('return 7;', cold['source_context'])
        self.assertTrue(cold['work_performed']['resident_warmed'])
        self.assertFalse(cold['work_performed']['resident_cache_hit'])
        self.assertTrue(self.run_ce('query', 'readLimit', root=roots[0])['work_performed']['resident_hit'])
        # Changes to a standby project are still observed before its next query.
        (roots[1] / 'limits.js').write_text('export function readLimit() { return 999; }\n')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            run = subprocess.run([str(fixtures.TOOL), 'query', 'readLimit', '--root', str(roots[1]), '--home', str(self.home)],
                                 capture_output=True, text=True, timeout=35)
            if run.returncode == 0 and 'return 999;' in json.loads(run.stdout)['source_context']:
                break
            if run.returncode == 0:
                self.assertNotIn('return 101;', json.loads(run.stdout)['source_context'], 'standby wake returned stale source')
            if run.returncode:
                # A cold reader independently qualifies the generation. It may
                # refuse before the watcher has observed this exact file edit.
                self.assertIn(json.loads(run.stderr)['error']['code'], {'project-updating', 'project-busy', 'service-busy', 'source-read-hash-mismatch', 'resident-not-qualified'})
            time.sleep(0.05)
        else:
            self.fail('standby project did not refresh')
        self.assertLessEqual(len(self.run_ce('service-status')['resident_projects']), 5)
        self.stop()
        restarted = self.run_ce('service-start')
        self.assertEqual(len(restarted['projects']), 16)
        self.assertLessEqual(len(restarted['resident_projects']), 5)
        self.assertIn('return 115;', self.run_ce('query', 'readLimit', root=roots[15])['source_context'])

    def test_concurrent_init_does_not_lose_enrollment_or_restart(self):
        roots = [self.root, self.project('other')]
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda root: self.run_ce('init', root=root), roots))
        self.assertEqual(sum(result['daemon_started'] for result in results), 1)
        self.assertEqual(len({result['service']['instance_id'] for result in results}), 1)
        self.assertEqual({entry['root'] for entry in json.loads(self.registry.read_text())['projects']}, set(map(str, roots)))
        for root in roots:
            self.assertTrue(self.run_ce('query', 'readLimit', '--service', root=root)['ok'])

    def test_detach_preserves_data_and_other_project_then_reconnects(self):
        first = self.run_ce('init')
        other = self.project('other', 91)
        self.run_ce('init', root=other)
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        binding = (workspace / 'state.json').read_bytes()
        source = (self.root / 'limits.js').read_bytes()
        query = self.run_ce('query', 'readLimit')
        history = self.home / 'service/history/telemetry.sqlite'
        history_inode = history.stat().st_ino
        detached = self.run_ce('detach')
        self.assertEqual(detached['state'], 'detached')
        self.assertTrue(detached['data_preserved'])
        self.assertEqual((workspace / 'state.json').read_bytes(), binding)
        self.assertEqual((self.root / 'limits.js').read_bytes(), source)
        self.assertTrue((workspace / 'index').is_dir())
        status = self.run_ce('service-status')
        self.assertEqual(status['instance_id'], first['service']['instance_id'])
        self.assertEqual(status['projects'], [str(other)])
        self.assertEqual({item['root'] for item in status['resident_projects']}, {str(other)})
        self.assertNotIn(str(self.root), {item['root'] for item in status['telemetry']['project_states']})
        self.assertEqual(history.stat().st_ino, history_inode)
        self.assertEqual(self.run_ce('query', 'readLimit', expected=2)['error']['code'], 'project-detached')
        self.assertEqual(self.run_ce('watch', '--samples', '1', expected=2)['error']['code'], 'project-detached')
        self.assertEqual(self.run_ce('detach')['state'], 'detached')
        self.assertEqual(self.run_ce('init', '--manage-index', expected=2)['error']['code'], 'invalid-arguments')
        self.assertEqual(self.run_ce('query', 'readLimit', expected=2)['error']['code'], 'project-detached')
        self.assertIn('return 91', self.run_ce('query', 'readLimit', root=other)['source_context'])
        connected = self.run_ce('init')
        self.assertEqual(connected['service']['instance_id'], first['service']['instance_id'])
        self.assertTrue(connected['service_enrolled'])
        self.assertEqual(self.run_ce('query', 'readLimit')['snapshot_id'], query['snapshot_id'])

    def test_detach_last_project_survives_service_restart(self):
        self.run_ce('init')
        self.run_ce('detach')
        self.assertEqual(json.loads(self.registry.read_text())['projects'], [])
        self.stop()
        self.assertTrue(self.run_ce('service-start')['started'])
        self.assertEqual(self.run_ce('service-status')['projects'], [])
        self.assertEqual(self.run_ce('status', expected=2)['error']['code'], 'project-detached')
        self.assertTrue(self.run_ce('init')['service_enrolled'])

    def test_detach_one_shot_and_uninitialized_root(self):
        missing = self.run_ce('detach', expected=2)
        self.assertFalse(missing['ok'])
        self.assertFalse(self.home.exists())
        self.run_ce('init', '--no-service')
        self.assertEqual(self.run_ce('detach')['state'], 'detached')
        self.assertFalse(self.endpoint.exists())
        self.assertTrue(self.run_ce('init', '--no-service')['local_ready'])

    def test_detach_releases_capacity(self):
        roots = [self.root] + [self.project(f'capacity-{i}') for i in range(5)]
        for root in roots[:5]:
            self.run_ce('init', root=root)
        self.run_ce('detach')
        self.assertTrue(self.run_ce('init', root=roots[5])['service_enrolled'])
        self.assertEqual(len(self.run_ce('service-status')['projects']), 5)

    def test_incomplete_detach_intent_fences_only_that_project(self):
        first = self.run_ce('init')
        other = self.project('other', 93)
        self.run_ce('init', root=other)
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        # Simulate interruption between private lifecycle mkdir and intent save.
        (workspace / 'lifecycle').mkdir(mode=0o700)
        status = self.await_status(lambda s: str(self.root) not in s['projects'])
        self.assertEqual(status['instance_id'], first['service']['instance_id'])
        self.assertIn('return 93', self.run_ce('query', 'readLimit', root=other)['source_context'])
        self.assertEqual(self.run_ce('init', expected=2)['error']['code'], 'project-detached')
        self.assertEqual(self.run_ce('detach')['state'], 'detached')
        self.assertTrue(self.run_ce('init')['service_enrolled'])

    def test_stop_and_restart_preserve_registry(self):
        first = self.run_ce('init')
        original = self.registry.read_bytes()
        self.stop()
        restarted = self.run_ce('service-start')
        self.assertTrue(restarted['started'])
        self.assertNotIn('telemetry', restarted)
        self.assertNotEqual(first['service']['instance_id'], restarted['instance_id'])
        self.await_status(lambda status: str(self.root) in status['projects'])
        existing = self.run_ce('service-start')
        self.assertFalse(existing['started'])
        self.assertNotIn('telemetry', existing)
        self.assertEqual(self.registry.read_bytes(), original)
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])

    def test_no_service_is_explicit_one_shot(self):
        result = self.run_ce('init', '--no-service')
        self.assertFalse(result['daemon_started'])
        self.assertFalse(self.registry.exists())
        self.assertFalse(self.endpoint.exists())
        self.assertTrue(self.run_ce('query', 'readLimit')['ok'])

    def test_registered_reads_default_to_resident_without_fallback(self):
        self.run_ce('init')
        query = self.run_ce('query', 'readLimit')
        self.assertEqual(query['transport'], 'local-socket-v1')
        self.assertTrue(query['work_performed']['resident_hit'])
        self.stop()
        self.assertEqual(self.run_ce('query', 'readLimit', expected=2)['error']['code'], 'service-unavailable')

    def test_registry_field_types_fail_closed_without_losing_local_index(self):
        self.run_ce('init')
        original = json.loads(self.registry.read_text())
        for key in ['schema_version', 'home_identity', 'projects']:
            value = {**original, key: 42}
            self.registry.write_text(json.dumps(value))
            result = self.run_ce('init', expected=1)
            self.assertTrue(result['local_ready'])
            self.assertEqual(result['service']['error']['code'], 'registry-invalid')
            self.assertEqual(json.loads(self.registry.read_text()), value)
        self.registry.write_text(json.dumps(original))

    def test_replaced_root_is_not_rebound_on_restart(self):
        self.run_ce('init')
        self.stop()
        before = self.registry.read_bytes()
        self.root.rename(self.base / 'previous-root')
        self.project('project', 99)
        self.run_ce('service-start')
        status = self.await_status(lambda status: str(self.root) in status['enrollment_errors'])
        self.assertEqual(status['enrollment_errors'][str(self.root)], 'registry-binding-changed')
        self.assertNotIn(str(self.root), status['projects'])
        self.assertEqual(self.registry.read_bytes(), before)
        self.assertEqual(self.run_ce('query', 'readLimit', '--service', expected=2)['error']['code'], 'service-project-denied')

    def test_corrupt_registry_does_not_break_admitted_reads(self):
        first = self.run_ce('init')
        self.registry.write_text('{}')
        status = self.await_status(lambda status: bool(status['registry_error']))
        self.assertEqual(status['registry_error'], 'registry-invalid')
        self.assertEqual(status['instance_id'], first['service']['instance_id'])
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])
        other = self.project('other')
        result = self.run_ce('init', root=other, expected=1)
        self.assertTrue(result['local_ready'])
        self.assertFalse(result['service_enrolled'])
        self.assertEqual(result['service']['error']['code'], 'registry-invalid')
        self.assertEqual(self.registry.read_text(), '{}')
        # A broken registry cannot safely decide between standalone and service
        # routing. Explicit admitted service reads above remain available.
        self.assertEqual(self.run_ce('query', 'readLimit', root=other, expected=2)['error']['code'], 'registry-invalid')

    def test_unavailable_registered_root_does_not_block_another(self):
        self.run_ce('init')
        self.stop()
        self.root.rename(self.base / 'removed')
        other = self.project('other')
        result = self.run_ce('init', root=other)
        self.assertTrue(result['service_enrolled'])
        status = self.await_status(lambda status: str(self.root) in status['enrollment_errors'])
        self.assertNotIn(str(self.root), status['projects'])
        self.assertTrue(self.run_ce('query', 'readLimit', '--service', root=other)['ok'])

    def test_explicit_service_is_never_restarted_or_reconfigured(self):
        self.run_ce('init', '--no-service')
        child = subprocess.Popen([str(fixtures.TOOL), 'serve', '--root', str(self.root), '--home', str(self.home)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def reap():
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
            try:
                child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                self.fail('explicit service did not stop')
        self.addCleanup(reap)
        deadline = time.monotonic() + 10
        while not self.endpoint.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        before = self.run_ce('service-status')
        other = self.project('other')
        result = self.run_ce('init', root=other, expected=1)
        self.assertTrue(result['local_ready'])
        self.assertEqual(result['service']['error']['code'], 'service-mode-conflict')
        after = self.run_ce('service-status')
        self.assertEqual(before['instance_id'], after['instance_id'])
        self.assertEqual(after['projects'], [str(self.root)])
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])
        self.stop()

    def stale_enrollment_response(self, code="project-detached", change_lifecycle=False):
        self.run_ce('init')
        ready = self.run_ce('service-status')
        self.stop()
        self.run_ce('detach')
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        expected = 1 if change_lifecycle or code != 'project-detached' else 0
        replies = 0
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(self.endpoint))
            self.endpoint.chmod(0o600)
            listener.listen(4)
            listener.settimeout(0.05)
            try:
                with ThreadPoolExecutor(max_workers=1) as pool:
                    pending = pool.submit(self.run_ce, 'init', expected=expected)
                    deadline = time.monotonic() + 10
                    while not pending.done() and time.monotonic() < deadline:
                        try:
                            connection, _ = listener.accept()
                        except socket.timeout:
                            continue
                        with connection:
                            connection.settimeout(3)
                            data = b''
                            while not data.endswith(b'\n'):
                                part = connection.recv(8192)
                                self.assertTrue(part)
                                data += part
                            frame = json.loads(data)
                            self.assertEqual(frame['command'], 'service-status')
                            if change_lifecycle and replies == 0:
                                # Another lifecycle transition invalidates this init,
                                # even if its final phase happens to be active again.
                                path = workspace / 'lifecycle/state.json'
                                lifecycle = json.loads(path.read_text())
                                lifecycle['revision'] += 1
                                path.write_text(json.dumps(lifecycle))
                            status = dict(ready)
                            if replies < 2:
                                status.update(projects=[], qualified_projects=[], resident_projects=[],
                                              enrollment_errors={str(self.root): code})
                            response = {key: frame[key] for key in
                                ['schema_version', 'request_id', 'observer_version', 'core_version']}
                            response.update(ok=True, result=status)
                            connection.sendall((json.dumps(response) + '\n').encode())
                            replies += 1
                    result = pending.result(timeout=5)
            finally:
                self.endpoint.unlink(missing_ok=True)
        if expected:
            self.assertEqual(replies, 1, 'unrelated or changed-lifecycle refusal was retried')
            self.assertFalse(result['service_enrolled'])
            self.assertEqual(result['service']['error']['code'], 'service-enrollment-failed')
        else:
            self.assertGreaterEqual(replies, 3, 'stale refusal was not followed by fresh admission')
            self.assertTrue(result['service_enrolled'])
            self.assertTrue(result['local_ready'])

    def test_reconnect_waits_for_stale_detached_observation(self):
        self.stale_enrollment_response()

    def test_reconnect_preserves_other_enrollment_refusals(self):
        self.stale_enrollment_response('registry-binding-changed')

    def test_reconnect_does_not_retry_after_another_lifecycle_transition(self):
        self.stale_enrollment_response(change_lifecycle=True)

    def test_unsafe_socket_is_preserved_and_init_reports_partial_failure(self):
        self.run_ce('init', '--no-service')
        self.endpoint.parent.mkdir(mode=0o700)
        self.endpoint.write_text('do not replace')
        self.endpoint.chmod(0o600)
        result = self.run_ce('init', expected=1)
        self.assertTrue(result['local_ready'])
        self.assertEqual(result['service']['error']['code'], 'service-socket-unsafe')
        self.assertEqual(self.endpoint.read_text(), 'do not replace')


if __name__ == '__main__':
    unittest.main()
