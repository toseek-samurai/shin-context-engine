#!/usr/bin/env python3
"""Bounded resident reads and automatic refresh in disposable private projects.

Usage: local_resident_test.py SHIN_CE PROFILE_DIR SHIN_CORE
Readiness and completed maintenance ticks, never a fixed sleep or latency target,
are the proof conditions. Only init and the service may prepare these fixtures.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest


TOOL = Path(sys.argv.pop(1)).resolve()
REGISTRY = Path(sys.argv.pop(1)).resolve()
CORE = Path(sys.argv.pop(1)).resolve()
TRANSIENT = {'project-updating', 'service-busy'}
SOURCE_TRANSIENT = TRANSIENT | {'source-read-hash-mismatch'}


class LocalResidentTests(unittest.TestCase):
    def setUp(self):
        # Reserve shutdown time below the 60-second fixture budget.
        self.deadline = time.monotonic() + 50
        self.temp = tempfile.TemporaryDirectory(prefix='shin-resident-', dir='/tmp')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.home = self.base / 'home'
        self.endpoint = self.home / 'service/rpc.sock'
        self.roots = [self.base / 'project-a', self.base / 'project-b']
        self.request_id = 0
        self.child = None
        self.addCleanup(self.stop_child)
        self.identity = self.ce('--version')
        for root, value in zip(self.roots, [7107, 9909]):
            root.mkdir()
            (root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {value}; }}\n')
            # Use an otherwise admitted extension to exercise directory filtering.
            (root / 'build').mkdir()
            (root / 'build/generated.js').write_text('export function buildOnly() { return 31337; }\n')
            initialized = self.ce('init', '--offline', '--no-service', '--root', str(root),
                                  '--home', str(self.home), '--profile-dir', str(REGISTRY))
            self.assertTrue(initialized['local_ready'], initialized)
            self.assertFalse(initialized['daemon_started'], initialized)
        self.child = subprocess.Popen(
            [str(TOOL), 'serve', '--home', str(self.home),
             '--root', str(self.roots[0]), '--root', str(self.roots[1])],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, umask=0,
            env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        self.wait_for(self.reachable, 'explicit service startup', 10)
        self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
        self.wait_for(self.all_ready, 'both resident preparations', 15)

    def remaining(self, deadline=None):
        left = min(self.deadline, deadline or self.deadline) - time.monotonic()
        self.assertGreater(left, 0, 'resident fixture exceeded its bounded deadline')
        return left

    def ce(self, *args):
        result = subprocess.run([str(TOOL), *args], capture_output=True, text=True,
                                timeout=min(15, self.remaining()),
                                env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(result.stderr, result.stderr)
        self.assertLessEqual(len(result.stdout.encode()), 65536)
        return json.loads(result.stdout)

    def stop_child(self):
        if self.child is None:
            return
        if self.child.poll() is None:
            self.child.terminate()
        try:
            self.child.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.communicate(timeout=2)
            self.fail('test service did not terminate within its shutdown bound')

    def wait_for(self, probe, label, seconds):
        deadline = min(self.deadline, time.monotonic() + seconds)
        last = None
        while time.monotonic() < deadline:
            last = probe()
            if last:
                return last
            # Retry pacing only: the observed condition above determines success.
            time.sleep(min(0.025, self.remaining(deadline)))
        self.fail(f'{label} did not become observable before its deadline: {last!r}')

    def rpc(self, command, root=None, **extra):
        self.request_id += 1
        identifier = f'resident-{self.request_id}'
        frame = {'schema_version': 'shin.local-rpc.v1', 'request_id': identifier,
                 'observer_version': self.identity['version'],
                 'core_version': self.identity['core_version'], 'command': command,
                 'root': str(root or self.roots[0]), 'query': '', 'path': '',
                 'record': '', 'snapshot': '', **extra}
        deadline = time.monotonic() + min(5, self.remaining())
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(self.remaining(deadline))
            client.connect(str(self.endpoint))
            client.sendall((json.dumps(frame) + '\n').encode())
            data = bytearray()
            while not data.endswith(b'\n'):
                client.settimeout(self.remaining(deadline))
                part = client.recv(8192)
                self.assertTrue(part, f'{command} disconnected without a complete response')
                data.extend(part)
                self.assertLessEqual(len(data), 128 * 1024)
        response = json.loads(data)
        self.assertEqual(response['request_id'], identifier, response)
        return response

    def reachable(self):
        if self.child.poll() is not None:
            self.fail('service failed startup: ' + repr(self.child.communicate(timeout=min(3, self.remaining()))))
        if not self.endpoint.exists():
            return None
        try:
            return self.service_status()
        except (OSError, ValueError):
            return None

    def service_status(self):
        response = self.rpc('service-status')
        self.assertTrue(response['ok'], response)
        result = response['result']
        self.assertTrue(result['service_running'], result)
        self.assertEqual(result['enrollment_mode'], 'explicit')
        self.assertEqual(result['resident_retention_bytes'], 768 * 1024 * 1024)
        self.assertIsInstance(result['resident_memory_limited'], bool)
        self.assertGreaterEqual(result['resident_memory_evictions'], 0)
        return result

    def residents(self, status):
        items = status['resident_projects']
        self.assertEqual(len({item['root'] for item in items}), len(items))
        self.assertTrue({item['root'] for item in items} <= {str(root) for root in self.roots})
        for item in items:
            self.assertIsInstance(item['pid'], int)
            self.assertGreater(item['pid'], 0)
            self.assertTrue(item['rss_bytes'] is None or
                            (isinstance(item['rss_bytes'], int) and item['rss_bytes'] >= 0))
        return {item['root']: item for item in items}

    def resident(self, status, root):
        item = self.residents(status).get(str(root))
        if item is None or not item['ready'] or item['busy']:
            return None
        preparation = item['preparation']
        self.assertIsInstance(preparation['snapshot_id'], str)
        self.assertTrue(preparation['snapshot_id'])
        self.assertGreater(preparation['payload_bytes'], 0)
        self.assertGreaterEqual(preparation['preparation_ms'], 0)
        return item

    def all_ready(self):
        status = self.service_status()
        return status if all(self.resident(status, root) for root in self.roots) else None

    def ready_request(self, command, root, **extra):
        def probe():
            response = self.rpc(command, root, **extra)
            if response['ok']:
                return response['result']
            self.assert_refusal(response, TRANSIENT)
            return None
        return self.wait_for(probe, f'{command} resident readiness', 10)

    def assert_refusal(self, response, codes):
        self.assertFalse(response['ok'], response)
        self.assertIn(response['error']['code'], codes, response)
        for value in [response, response.get('result', {})]:
            self.assertNotIn('source_context', value, response)
            self.assertNotIn('source_inventory', value, response)

    def assert_contract(self, result, root, selected=True):
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['root'], str(root))
        contract = result['freshness_contract']
        self.assertEqual(contract['mode'], 'background-qualified-published-generation')
        self.assertIs(contract['whole_project_checked_during_request'], False)
        self.assertIs(contract['selected_sources_verified'], True)
        self.assertNotIn('readiness', result)
        self.assertIsInstance(result['background_readiness'], dict)
        self.assertIs(result['readiness_assessed'], False)
        work = result['work_performed']
        self.assertIs(work['resident_hit'], True)
        self.assertEqual(work['full_generations'], 0)
        self.assertIsInstance(work['request_sequence'], int)
        self.assertGreater(work['request_sequence'], 0)
        self.assertGreaterEqual(work['selected_files'], 0)
        self.assertGreaterEqual(work['selected_source_bytes'], 0)
        if selected:
            self.assertGreater(work['selected_files'], 0)
            self.assertGreater(work['selected_source_bytes'], 0)

    def assert_body(self, result, expected, excluded):
        body = result['source_context']
        self.assertIsInstance(body, str)
        self.assertTrue(body.startswith('SHIN '), body)
        header = json.loads(body.splitlines()[0][5:])
        self.assertEqual(header['snapshot_id'], result['snapshot_id'])
        self.assertIn(expected, body)
        self.assertNotIn(excluded, body)
        self.assertNotIn('buildOnly', body)

    def test_resident_reuse_refresh_followup_and_project_isolation(self):
        first, second = self.roots
        initial = self.service_status()
        preparation = self.resident(initial, first)['preparation']
        other_preparation = self.resident(initial, second)['preparation']
        status = self.ready_request('status', first)
        self.assert_contract(status, first, selected=False)
        original = self.ready_request('query', first, query='readLimit')
        self.assert_contract(original, first)
        self.assert_body(original, 'return 7107;', 'return 9909;')
        snapshot = original['snapshot_id']
        self.assertEqual(snapshot, preparation['snapshot_id'])
        self.assertEqual(original['document_context']['snapshot_id'], snapshot)
        documents = original['document_context']
        self.assertTrue(documents['evidence'], documents)
        self.assertIn('limits.js', {row['path'] for row in documents['evidence']})
        self.assertLessEqual(len(documents['evidence']), 4)
        self.assertGreaterEqual(original['phase_ms']['documents'], 0)
        self.assertGreaterEqual(original['phase_ms']['source'], 0)
        self.assertGreaterEqual(original['phase_ms']['source_collection'], 0)
        baseline = subprocess.run([str(CORE), '--root', str(first), '--out-dir', original['index'],
            '--profile', documents['profile'], '--profile-dir', str(REGISTRY), 'brief',
            '--projection', 'compact', '--limit', '4', '--require-current', '--', 'readLimit'],
            capture_output=True, text=True, timeout=min(10, self.remaining()), check=True)
        expected = json.loads(baseline.stdout)
        for key in ['evidence', 'ranked_paths', 'query_coverage', 'evidence_budget', 'verification_paths']:
            self.assertEqual(documents[key], expected[key], key)
        repeated = self.ready_request('query', first, query='readLimit')
        self.assert_contract(repeated, first)
        self.assertEqual(repeated['work_performed']['request_sequence'],
                         original['work_performed']['request_sequence'] + 1)
        for key in ['snapshot_id', 'state', 'background_readiness', 'source_context']:
            self.assertEqual(repeated[key], original[key], key)
        self.assertEqual(self.resident(self.service_status(), first)['preparation'], preparation)
        literal = self.ready_request('query', first, query='--limit')
        self.assertEqual(literal['document_context']['query'], '--limit')

        inventory = self.ready_request('inspect', first, path='limits.js', snapshot=snapshot)
        self.assert_contract(inventory, first)
        self.assertEqual(inventory['source_inventory']['snapshot_id'], snapshot)
        record = next(row['id'] for row in inventory['source_inventory']['records'] if row['name'] == 'readLimit')
        for selector in [{'path': 'limits.js'}, {'record': record}]:
            read = self.ready_request('read', first, snapshot=snapshot, **selector)
            self.assert_contract(read, first)
            self.assertEqual(read['snapshot_id'], snapshot)
            self.assertTrue(read['native_read_complete'])
            self.assert_body(read, 'return 7107;', 'return 9909;')
        other = self.ready_request('query', second, query='readLimit')
        self.assert_contract(other, second)
        self.assert_body(other, 'return 9909;', 'return 7107;')
        self.assertNotEqual(other['snapshot_id'], snapshot)
        other_read = self.ready_request('read', second, path='limits.js', snapshot=other['snapshot_id'])
        self.assert_contract(other_read, second)
        self.assert_body(other_read, 'return 9909;', 'return 7107;')
        for command in ['inspect', 'read']:
            self.assert_refusal(self.rpc(command, second, path='limits.js', snapshot=snapshot),
                                {'source-snapshot-mismatch'})
        unregistered = self.base / 'unregistered'
        unregistered.mkdir()
        self.assert_refusal(self.rpc('query', unregistered, query='readLimit'), {'service-project-denied'})

        before_artifact = self.ready_request('query', first, query='readLimit')
        baseline = self.service_status()
        ticks = next(row['ticks'] for row in baseline['telemetry']['project_states'] if row['root'] == str(first))
        (first / 'build/generated.js').write_text('export function buildOnly() { return 42424; }\n')
        artifact = first / 'build/transient.js'
        artifact.write_text('export function ignoredNow() { return 1; }\n')
        artifact.unlink()

        def artifact_observed():
            current = self.service_status()
            for root, expected in [(first, preparation), (second, other_preparation)]:
                worker = self.resident(current, root)
                self.assertIsNotNone(worker, 'ignored artifact evicted ready resident')
                self.assertEqual(worker['preparation'], expected, 'ignored artifact replaced preparation')
            row = next(row for row in current['telemetry']['project_states'] if row['root'] == str(first))
            # Two completions ensure at least one maintenance pass started after
            # the edit, even if the baseline sampled an already-running pass.
            return current if row['ticks'] >= ticks + 2 and row['phase'] == 'current' else None

        self.wait_for(artifact_observed, 'completed maintenance after ignored artifacts', 12)
        after_artifact = self.ready_request('query', first, query='readLimit')
        self.assert_contract(after_artifact, first)
        self.assertEqual(after_artifact['work_performed']['request_sequence'],
                         before_artifact['work_performed']['request_sequence'] + 1)
        self.assertEqual(after_artifact['snapshot_id'], snapshot)
        self.assertEqual(after_artifact['source_context'], before_artifact['source_context'])

        # No re-init, refresh command, manual build or publication after this edit.
        (first / 'limits.js').write_text('export function readLimit() { return 7717; }\n')

        def updated_query():
            response = self.rpc('query', first, query='readLimit')
            if not response['ok']:
                self.assert_refusal(response, SOURCE_TRANSIENT)
                return None
            result = response['result']
            self.assert_contract(result, first)
            self.assert_body(result, 'return 7717;', 'return 7107;')
            self.assertNotEqual(result['snapshot_id'], snapshot)
            return result

        immediate = updated_query()  # Even the first successful response must not expose the old body.
        updated = immediate or self.wait_for(updated_query, 'automatic resident source refresh', 20)

        def updated_preparation():
            current = self.service_status()
            untouched = self.resident(current, second)
            self.assertIsNotNone(untouched, 'project A refresh evicted project B')
            self.assertEqual(untouched['preparation'], other_preparation)
            ready = self.resident(current, first)
            return ready if ready and ready['preparation']['snapshot_id'] == updated['snapshot_id'] else None

        self.wait_for(updated_preparation, 'published replacement preparation', 5)
        self.assert_refusal(self.rpc('read', first, path='limits.js', snapshot=snapshot),
                            {'source-snapshot-mismatch'})
        current_read = self.ready_request('read', first, path='limits.js', snapshot=updated['snapshot_id'])
        self.assert_contract(current_read, first)
        self.assert_body(current_read, 'return 7717;', 'return 7107;')
        untouched = self.ready_request('query', second, query='readLimit')
        self.assert_contract(untouched, second)
        self.assert_body(untouched, 'return 9909;', 'return 7717;')
        self.assertEqual(untouched['snapshot_id'], other['snapshot_id'])
        self.assertEqual(untouched['work_performed']['request_sequence'],
                         other_read['work_performed']['request_sequence'] + 1)
        stopped = self.rpc('service-stop')
        self.assertTrue(stopped['ok'], stopped)
        self.assertTrue(stopped['result']['stop_requested'])
        stdout, stderr = self.child.communicate(timeout=min(5, self.remaining()))
        self.assertEqual(self.child.returncode, 0, stdout + stderr)
        self.assertTrue(json.loads(stdout)['service_stopped'])
        self.assertFalse(self.endpoint.exists())


if __name__ == '__main__':
    unittest.main()
