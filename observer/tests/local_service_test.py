#!/usr/bin/env python3
"""Native Unix-socket service and process-worker boundaries; no runtime Python."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time
import unittest

import local_project_test as fixtures


class LocalServiceTests(unittest.TestCase):
    run_ce = fixtures.LocalProjectTests.run_ce
    init = fixtures.LocalProjectTests.init
    file_map = fixtures.LocalProjectTests.file_map
    workspace = fixtures.LocalProjectTests.workspace

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-ipc-', dir='/tmp')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.base / 'project'
        self.root.mkdir()
        (self.root / 'limits.js').write_text('export function readLimit() { return 7; }\n')
        (self.root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
        self.home = self.base / 'home'
        self.endpoint = self.home / 'service/rpc.sock'
        self.identity = json.loads(subprocess.check_output([str(fixtures.TOOL), '--version'], text=True))
        self.roots = [self.root]
        self.init()

    def start(self, tool=fixtures.TOOL, ready=True):
        args = [str(tool), 'serve', '--home', str(self.home)]
        for root in self.roots:
            args += ['--root', str(root)]
        child = subprocess.Popen(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, umask=0,
                                 env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        def cleanup():
            if child.poll() is None:
                child.send_signal(signal.SIGCONT)
                child.terminate()
            try:
                child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                self.fail('native service failed to stop')
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if child.poll() is not None:
                self.fail('service failed startup: ' + str(child.communicate()))
            if self.endpoint.exists():
                self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
                try:
                    status = self.raw('service-status')
                    readers = status.get('result', {}).get('resident_projects', [])
                    prepared = {row['root'] for row in readers if row['ready']}
                    if status.get('ok') and (not ready or prepared == {str(root) for root in self.roots}):
                        return child
                except (OSError, ValueError):
                    pass
            time.sleep(0.03)
        self.fail('service did not become reachable')

    def frame(self, command, root=None, **extra):
        return {'schema_version': 'shin.local-rpc.v1', 'request_id': 'test-request',
                'observer_version': self.identity['version'], 'core_version': self.identity['core_version'],
                'command': command, 'root': str(root or self.root), 'query': '', 'path': '', 'record': '',
                'snapshot': '', **extra}

    def test_previous_generator_is_rebuilt_before_the_first_read(self):
        self.previous_generator_rebuild('shin-generator-v22-compact-lexical')

    def test_previous_unicode_generator_is_rebuilt_before_the_first_read(self):
        self.previous_generator_rebuild('shin-generator-v23-unicode-lexical')

    def previous_generator_rebuild(self, previous_generator):
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        manifest = workspace / 'index/docs/kb/generated/manifest.json'
        original = manifest.read_text()
        self.assertIn('shin-generator-v24-objective-cpp', original)
        prior = original.replace('shin-generator-v24-objective-cpp', previous_generator)
        manifest.write_text(prior)
        value = 1469598103934665603
        for byte in prior.encode():
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        binding_path = workspace / 'state.json'
        binding = json.loads(binding_path.read_text())
        binding['generator_version'] = previous_generator
        binding['prepared_snapshot'] = f'fnv1a64:{value:016x}'
        binding_path.write_text(json.dumps(binding))
        sentinel = self.home / 'retained-user-state'
        sentinel.write_text('not derived from project sources')
        before_sources = self.file_map(self.root)
        self.start()
        response = self.raw('query', query='readLimit')
        self.assertTrue(response['ok'], response)
        self.assertIn('return 7;', response['result']['source_context'])
        current = json.loads(manifest.read_text())
        self.assertEqual(current['generator_version'], 'shin-generator-v24-objective-cpp')
        self.assertEqual(json.loads(binding_path.read_text())['generator_version'], current['generator_version'])
        self.assertEqual(sentinel.read_text(), 'not derived from project sources')
        self.assertEqual(before_sources, self.file_map(self.root))

    def test_headless_status_is_read_only_and_json_remains_default(self):
        def text_status(command, *extra, home=None):
            return subprocess.run([str(fixtures.TOOL), command, '--text', '--home', str(home or self.home),
                                   '--root', str(self.root), *extra], capture_output=True, text=True,
                                  timeout=10, env={**os.environ, 'PATH': ''})
        child = self.start()
        before = self.raw('service-status')['result']
        overview = text_status('service-status')
        self.assertEqual(overview.returncode, 0, overview.stderr)
        self.assertIn('Shin service: running', overview.stdout)
        self.assertIn(str(self.root), overview.stdout)
        project = text_status('status', '--service')
        self.assertEqual(project.returncode, 0, project.stderr)
        self.assertIn('Search: ready', project.stdout)
        after = self.raw('service-status')['result']
        self.assertEqual(before['instance_id'], after['instance_id'])
        self.assertEqual(before['telemetry']['request_metrics'], after['telemetry']['request_metrics'])
        self.assertEqual(text_status('service-status', '--text').returncode, 2)
        child.terminate()
        child.communicate(timeout=15)
        stopped = text_status('service-status')
        self.assertEqual(stopped.returncode, 2)
        self.assertIn('Shin status unavailable:', stopped.stderr)
        self.assertFalse(self.endpoint.exists())
        missing = self.base / 'missing-home'
        self.assertEqual(text_status('service-status', home=missing).returncode, 2)
        self.assertFalse(missing.exists())

    def test_reset_clients_cannot_stop_shared_service(self):
        child = self.start()
        before = self.raw('service-status')['result']
        frame = (json.dumps(self.frame('service-status')) + '\n').encode()
        for index in range(256):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(1)
                try:
                    client.connect(str(self.endpoint))
                    # Exercise peers leaving before a request, during framing,
                    # and before their reply is consumed.
                    if index % 3:
                        client.sendall(frame if index % 3 == 1 else frame[:len(frame) // 2])
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                except OSError:
                    # A bounded backlog may reject an individual connection.
                    # It must not terminate the service or replace its identity.
                    pass
            if index % 8 == 0:
                time.sleep(0.01)
                self.assertIsNone(child.poll(), 'a reset client terminated the shared service')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.assertIsNone(child.poll(), 'a reset client terminated the shared service')
            status = subprocess.run([str(fixtures.TOOL), 'service-status', '--home', str(self.home)],
                                    text=True, capture_output=True, timeout=3)
            if status.returncode == 0:
                after = json.loads(status.stdout)
                break
            self.assertEqual(status.returncode, 2, status.stderr)
            self.assertIn(json.loads(status.stderr)['error']['code'], ('service-unavailable', 'service-disconnected'))
            time.sleep(0.01)
        else:
            self.fail('service did not become reachable after the reset-client burst')
        self.assertEqual(before['instance_id'], after['instance_id'])
        self.assertIsNone(child.poll())
        self.assertIn('return 7;', self.raw('query', query='readLimit')['result']['source_context'])

    def connect(self):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(client.close)
        client.settimeout(10)
        client.connect(str(self.endpoint))
        return client

    def receive(self, client):
        data = b''
        while not data.endswith(b'\n'):
            part = client.recv(8192)
            self.assertTrue(part, data)
            data += part
            self.assertLessEqual(len(data), 128 * 1024)
        return json.loads(data)

    def raw(self, command, root=None, **extra):
        with self.connect() as client:
            client.sendall((json.dumps(self.frame(command, root, **extra)) + '\n').encode())
            return self.receive(client)

    def test_five_projects_native_queries_followup_and_automatic_refresh(self):
        for index in range(1, 5):
            root = self.base / f'project-{index}'
            root.mkdir()
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {7 + index}; }}\n')
            (root / 'README.md').write_text(f'# Project {index}\n\nreadLimit owns the ceiling.\n')
            self.init(root=root)
            self.roots.append(root)
        child = self.start()
        self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
        for index, root in enumerate(self.roots):
            result = self.run_ce('query', 'readLimit', '--service', root=root)
            self.assertEqual(result['transport'], 'local-socket-v1')
            self.assertEqual(result['root'], str(root))
            self.assertIn(f'return {7 + index};', result['source_context'])
            read = self.run_ce('read', 'limits.js', '--snapshot', result['snapshot_id'], '--service', root=root)
            self.assertTrue(read['ok'])
        (self.root / 'limits.js').write_text('export function readLimit() { return 77; }\n')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            result = self.raw('query', query='readLimit')
            if result.get('ok') and 'return 77;' in result['result']['source_context']:
                break
            time.sleep(0.1)
        else:
            self.fail('service did not refresh before native query')
        self.assertIn('return 8;', self.run_ce('query', 'readLimit', '--service', root=self.roots[1])['source_context'])
        stopped = self.run_ce('service-stop')
        self.assertTrue(stopped['stop_requested'])
        self.assertNotIn('telemetry', stopped)
        stdout, stderr = child.communicate(timeout=15)
        self.assertEqual(child.returncode, 0, stderr)
        self.assertTrue(json.loads(stdout)['service_stopped'])
        self.assertFalse(self.endpoint.exists())
        self.assertFalse(self.run_ce('status')['watch']['running'])

    def test_blocked_project_does_not_block_other_queries_or_service_control(self):
        other = self.base / 'other-project'
        other.mkdir()
        (other / 'limits.js').write_text('export function readLimit() { return 99; }\n')
        (other / 'README.md').write_text('# Other limits\n\nreadLimit owns the ceiling.\n')
        self.init(root=other)
        self.roots.append(other)
        child = self.start()
        workspace = fixtures.LocalProjectTests.workspace(self)
        config = json.loads((workspace / 'config/state.json').read_text())
        response_seconds = 3

        def bounded_rpc(command, **extra):
            deadline = time.monotonic() + response_seconds
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(response_seconds)
                try:
                    client.connect(str(self.endpoint))
                    client.sendall((json.dumps(self.frame(command, **extra)) + '\n').encode())
                    data = b''
                    while not data.endswith(b'\n'):
                        remaining = deadline - time.monotonic()
                        self.assertGreater(remaining, 0, f'{command} exceeded its deadline')
                        client.settimeout(remaining)
                        part = client.recv(8192)
                        self.assertTrue(part, f'{command} disconnected without a response')
                        data += part
                        self.assertLessEqual(len(data), 128 * 1024)
                except socket.timeout:
                    self.fail(f'{command} did not respond within {response_seconds}s while project A remained locked')
            self.assertLess(time.monotonic(), deadline, f'{command} exceeded its deadline')
            return json.loads(data)

        with (workspace / 'index/docs/kb/.shin-context.lock').open('rb') as lock:
            try:
                deadline = time.monotonic() + response_seconds
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        self.assertLess(time.monotonic(), deadline, 'could not acquire project A index lock')
                        time.sleep(.02)
                # Keep A locked beyond its poll interval so its refresh is due.
                time.sleep(config['poll_interval_ms'] / 1000 + 1)
                with self.subTest(command='query', root='B'):
                    result = bounded_rpc('query', root=other, query='readLimit')
                    self.assertTrue(result['ok'], result)
                    self.assertEqual(result['result']['root'], str(other))
                    self.assertIn('return 99;', result['result']['source_context'])
                with self.subTest(command='service-status'):
                    result = bounded_rpc('service-status')
                    self.assertTrue(result['ok'], result)
                    self.assertTrue(result['result']['service_running'])
                with self.subTest(command='service-stop'):
                    result = bounded_rpc('service-stop')
                    self.assertTrue(result['ok'], result)
                    self.assertTrue(result['result']['stop_requested'])
                with self.subTest(command='shutdown'):
                    try:
                        output, error = child.communicate(timeout=response_seconds)
                    except subprocess.TimeoutExpired:
                        self.fail('service did not shut down while project A remained locked')
                    self.assertEqual(child.returncode, 0, output + error)
                    self.assertTrue(json.loads(output)['service_stopped'])
                    self.assertFalse(self.endpoint.exists())
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.communicate(timeout=response_seconds)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.communicate(timeout=response_seconds)

    def test_stop_during_explicit_root_initial_admission_exits_cleanly(self):
        other = self.base / 'other-project'
        other.mkdir()
        (other / 'README.md').write_text('# Other project\n')
        (other / 'limits.js').write_text('export function readLimit() { return 99; }\n')
        self.init(root=other)
        self.roots.append(other)
        workspace = fixtures.LocalProjectTests.workspace(self)
        response_seconds = 3

        def bounded_rpc(command):
            deadline = time.monotonic() + response_seconds
            with self.connect() as client:
                client.settimeout(response_seconds)
                try:
                    client.sendall((json.dumps(self.frame(command)) + '\n').encode())
                    data = b''
                    while not data.endswith(b'\n'):
                        remaining = deadline - time.monotonic()
                        self.assertGreater(remaining, 0, f'{command} exceeded its deadline')
                        client.settimeout(remaining)
                        part = client.recv(8192)
                        self.assertTrue(part, f'{command} disconnected without an acknowledgement')
                        data += part
                        self.assertLessEqual(len(data), 128 * 1024)
                except socket.timeout:
                    self.fail(f'{command} timed out during locked initial admission')
            self.assertLess(time.monotonic(), deadline, f'{command} exceeded its deadline')
            return json.loads(data)

        child = None
        with (workspace / 'index/docs/kb/.shin-context.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                # A cannot finish initial admission while this pre-start lock is held.
                child = self.start(ready=False)
                status = bounded_rpc('service-status')
                self.assertTrue(status['ok'], status)
                self.assertEqual(status['result']['enrollment_mode'], 'explicit')
                self.assertTrue(status['result']['service_running'])
                with self.subTest(command='service-stop'):
                    stopped = bounded_rpc('service-stop')
                    self.assertTrue(stopped['ok'], stopped)
                    self.assertTrue(stopped['result']['stop_requested'])
                with self.subTest(command='shutdown'):
                    try:
                        output, error = child.communicate(timeout=response_seconds)
                    except subprocess.TimeoutExpired:
                        self.fail('initial-admission shutdown waited for project A index lock')
                    self.assertEqual(child.returncode, 0, output + error)
                    self.assertFalse(error, error)
                    self.assertTrue(json.loads(output)['service_stopped'])
                    self.assertFalse(self.endpoint.exists())
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
                if child is not None and child.poll() is None:
                    child.terminate()
                    try:
                        child.communicate(timeout=response_seconds)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.communicate(timeout=response_seconds)

    def test_protocol_scope_and_write_denials_preserve_sources(self):
        self.start()
        before = self.file_map(self.root)
        cases = [self.frame('init'), self.frame('query', root=self.base),
                 self.frame('query', observer_version='2.0.0-dev.1'), self.frame('query', unexpected=True)]
        expected = ['service-method-denied', 'service-project-denied', 'service-version-mismatch', 'service-request-invalid']
        for request, code in zip(cases, expected):
            with self.connect() as client:
                client.sendall((json.dumps(request) + '\n').encode())
                result = self.receive(client)
                self.assertFalse(result['ok'])
                self.assertEqual(result['error']['code'], code)
        for text in ['{\n', '{"x":1,"x":2}\n', '{}\n{}\n', 'x' * 8193 + '\n']:
            with self.connect() as client:
                client.sendall(text.encode())
                self.assertFalse(self.receive(client)['ok'])
        self.assertEqual(before, self.file_map(self.root))
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])

    def test_status_telemetry_is_bounded_and_does_not_spawn_queries(self):
        self.start()
        status = self.raw('service-status')['result']
        baseline = status['telemetry']['requests_started']
        for _ in range(5):
            status = self.raw('service-status')['result']
            self.assertEqual(status['telemetry']['requests_started'], baseline)
            self.assertEqual(status['query_workers'], 0)
        telemetry = status['telemetry']
        self.assertEqual(telemetry['schema_version'], 'shin.local-service-telemetry.v1')
        self.assertFalse(telemetry['durable'])
        self.assertLessEqual(len(telemetry['samples']), 120)
        self.assertLessEqual(len(telemetry['events']), 32)
        self.assertEqual(telemetry['project_states'][0]['name'], 'project')
        self.assertNotIn('return 7', json.dumps(telemetry))
        self.run_ce('query', 'readLimit', '--service')
        status = self.raw('service-status')['result']['telemetry']
        self.assertEqual(status['requests_started'], baseline + 1)
        metrics = status['request_metrics']
        self.assertEqual(metrics['responses_ready'], 1)
        self.assertEqual(metrics['response_errors'], 0)
        self.assertEqual(metrics['query_sample_count'], 1)
        self.assertGreaterEqual(metrics['query_p50_ms'], 0)
        self.assertGreater(metrics['recent'][0]['response_bytes'], 0)
        measured = metrics['recent'][0]
        self.assertGreaterEqual(measured['service_duration_ms'], measured['duration_ms'])
        self.assertEqual(measured['reader_state'], 'warm')
        self.assertEqual(measured['purpose'], 'unclassified')
        self.assertGreaterEqual(measured['phase_ms']['source'], 0)
        self.assertEqual(metrics['model_attribution'], 'caller-reported-not-verified')
        self.assertNotIn('readLimit', json.dumps(metrics))
        self.assertNotIn(str(self.root), json.dumps(metrics))
        failed = self.raw('read', path='limits.js', snapshot='fnv1a64:0000000000000000')
        self.assertFalse(failed['ok'])
        metrics = self.raw('service-status')['result']['telemetry']['request_metrics']
        self.assertEqual(metrics['response_errors'], 1)
        self.assertEqual(metrics['unobserved'], 0)
        self.assertEqual(metrics['query_sample_count'], 1)
        self.assertEqual(metrics['recent'][-1]['error_code'], 'source-snapshot-mismatch')
        self.assertFalse(self.raw('query', root=self.base)['ok'])
        metrics = self.raw('service-status')['result']['telemetry']['request_metrics']
        self.assertEqual(metrics['rpc_admission_rejections'], 1)

    def test_updating_wait_is_bounded_and_disconnect_releases_capacity(self):
        self.start()
        workspace = fixtures.LocalProjectTests.workspace(self)
        (self.root / 'limits.js').write_text('export function readLimit() { return 73; }\n')
        deadline = time.monotonic() + 5
        while self.raw('service-status')['result']['qualified_projects']:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        with (workspace / 'index/docs/kb/.shin-context.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            refresh_deadline = time.monotonic() + 65
            cancelled = self.connect()
            cancelled.sendall((json.dumps(self.frame('query', query='readLimit', request_id='cancelled-wait')) + '\n').encode())
            deadline = time.monotonic() + 3
            while not self.raw('service-status')['result']['waiting_requests']:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            cancelled.close()
            deadline = time.monotonic() + 3
            while self.raw('service-status')['result']['waiting_requests']:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            with self.connect() as client:
                client.settimeout(8)
                start = time.monotonic()
                client.sendall((json.dumps(self.frame('query', query='readLimit', request_id='bounded-wait')) + '\n').encode())
                refused = self.receive(client)
                elapsed = time.monotonic() - start
                self.assertEqual(refused['error']['code'], 'project-updating')
                self.assertGreaterEqual(elapsed, 4.5)
                self.assertLess(elapsed, 7)
            metrics = self.raw('service-status')['result']['telemetry']['request_metrics']
            self.assertEqual(metrics['rpc_admission_rejections'], 2)
            self.assertEqual([r['error_code'] for r in metrics['recent']], ['client-disconnected', 'project-updating'])
            self.assertEqual(metrics['admission_waits'], 2)
            # Keep the existing lock fixture until the actual refresh deadline:
            # the final response never arrives, but its last stage must survive.
            while time.monotonic() < refresh_deadline:
                telemetry = self.raw('service-status')['result']['telemetry']
                timed_out = [r for r in telemetry.get('refresh_diagnostics', []) if r['error_code'] == 'refresh-timeout']
                if timed_out:
                    break
                time.sleep(.1)
            self.assertTrue(timed_out, telemetry)
            self.assertTrue(timed_out[-1]['last_stage'])
            self.assertGreaterEqual(timed_out[-1]['duration_ms'], 60000)
            history = self.history(lambda h: any(r['error_code'] == 'refresh-timeout' for r in h.get('refresh_diagnostics', [])))
            persisted = next(r for r in history['refresh_diagnostics'] if r['error_code'] == 'refresh-timeout')
            self.assertEqual(persisted['last_stage'], timed_out[-1]['last_stage'])
            fcntl.flock(lock, fcntl.LOCK_UN)
        # A confirmed timeout stays visible until a successful requalification.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = self.raw('service-status')['result']
            if status['qualified_projects'] and not any(p['active_issue'] for p in status['telemetry']['project_states']):
                break
            time.sleep(.05)
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])

    def test_updating_wait_recovers_and_preserves_original_snapshot(self):
        self.start()
        snapshot = self.raw('query', query='readLimit')['result']['snapshot_id']
        for value, method in [(71, 'query'), (72, 'read')]:
            (self.root / 'limits.js').write_text(f'export function readLimit() {{ return {value}; }}\n')
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                status = self.raw('service-status')['result']
                if not status['qualified_projects']:
                    break
                time.sleep(.01)
            else:
                self.fail('fixture did not enter background requalification')
            result = self.raw(method, query='readLimit' if method == 'query' else '',
                              path='limits.js' if method == 'read' else '', snapshot=snapshot if method == 'read' else '')
            if method == 'query':
                self.assertTrue(result['ok'], result)
                self.assertIn('return 71', result['result']['source_context'])
            else:
                self.assertEqual(result['error']['code'], 'source-snapshot-mismatch')
            metric = self.raw('service-status')['result']['telemetry']['request_metrics']['recent'][-1]
            self.assertGreater(metric['wait_ms'], 0)
            self.assertGreater(metric['admission_retries'], 0)
            self.assertEqual(metric['stage'], 'execution')
        self.assertEqual(self.raw('service-status')['result']['telemetry']['request_metrics']['rpc_admission_rejections'], 0)

    def test_early_admission_refusals_survive_restart_without_claiming_unknown_roots(self):
        child = self.start()
        self.assertEqual(self.raw('query', root=self.base)['error']['code'], 'service-project-denied')
        self.assertEqual(self.raw('query', unexpected='PRIVATE-CONTENT')['error']['code'], 'service-request-invalid')
        history = self.history(lambda h: h.get('admission_rejections') == 2)
        self.assertEqual(sum(row[3] for row in history['admission_cohorts']), 2)
        self.assertTrue(all(row[0] == '' for row in history['admission_cohorts']))
        metrics = self.raw('service-status')['result']['telemetry']['request_metrics']
        self.assertEqual(metrics['rpc_admission_rejections'], 2)
        self.assertEqual(metrics['unobserved'], 0)
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        path = self.endpoint.parent / 'history/telemetry.sqlite'
        self.assertNotIn(b'PRIVATE-CONTENT', path.read_bytes())
        self.assertNotIn(str(self.base).encode(), path.read_bytes())
        self.start()
        self.assertEqual(self.history()['admission_rejections'], 2)

    def test_correlation_is_explicit_and_does_not_cross_sessions_or_projects(self):
        child = self.start()
        session = 'a' * 64
        query_id = 'query-one'
        query = self.raw('query', query='readLimit', request_id=query_id, session_id=session, model_id='fixture/model-a')
        parent = hashlib.sha256(query_id.encode()).hexdigest()
        for current_session, expected in [(session, 'observed'), ('b' * 64, 'unverified')]:
            read = self.raw('read', path='limits.js', snapshot=query['result']['snapshot_id'],
                            session_id=current_session, parent_request_id=parent)
            self.assertTrue(read['ok'], read)
            row = self.raw('service-status')['result']['telemetry']['request_metrics']['recent'][-1]
            self.assertEqual(row['follow_up'], expected)
        self.assertEqual(self.history(lambda h: h.get('observed_follow_ups') == 1)['correlated_requests'], 3)
        history = self.history(lambda h: any(r['model_id'] == 'fixture/model-a' for r in h.get('model_cohorts', [])))
        cohort = next(r for r in history['model_cohorts'] if r['model_id'] == 'fixture/model-a')
        self.assertEqual(cohort, {'model_id': 'fixture/model-a', 'requests': 1, 'reported_sessions': 1})
        for metadata in ({'model_id': 'fixture/model-a'}, {'session_id': session, 'model_id': 'bad model'}):
            self.assertEqual(self.raw('query', **metadata)['error']['code'], 'service-request-invalid')
        cli = self.run_ce('query', 'readLimit', '--service', '--session-id', 'CLI-PRIVATE-SESSION', '--model-id', 'fixture/model-b')
        self.assertTrue(cli['request_trace']['session_correlated'])
        self.assertEqual(cli['request_trace']['model_id'], 'fixture/model-b')
        self.assertNotIn('CLI-PRIVATE-SESSION', json.dumps(self.raw('service-status')))
        self.history(lambda h: any(r['model_id'] == 'fixture/model-b' for r in h.get('model_cohorts', [])))
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        self.start()
        persisted = self.history(lambda h: bool(h.get('model_cohorts')))
        self.assertTrue(any(r['model_id'] == 'fixture/model-b' and r['requests'] == 1 for r in persisted['model_cohorts']))

        refused = self.raw('query', session_id='PRIVATE-SESSION')
        self.assertEqual(refused['error']['code'], 'service-request-invalid')
        self.assertNotIn('PRIVATE-SESSION', json.dumps(self.raw('service-status')['result']['telemetry']['request_metrics']))

    def test_diagnostic_history_and_v1_upgrade(self):
        child = self.start()
        self.run_ce('query', 'readLimit', '--service', '--diagnostic')
        history = self.history(lambda h: bool(h.get('request_cohorts')))
        cohort = history['request_cohorts'][0]
        self.assertEqual(len(cohort), 22)
        self.assertEqual(cohort[1:4], ['query', 'diagnostic', 'warm'])
        self.assertEqual(cohort[4:7], [1, 0, 1])
        self.assertGreaterEqual(cohort[7], cohort[9])
        self.assertEqual(cohort[16:], [1, 1, 1, 1, 1, 1])
        self.assertFalse(self.raw('query', diagnostic='invalid')['ok'])
        self.assertFalse(self.raw('service-status', diagnostic=True)['ok'])
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        path = self.endpoint.parent / 'history/telemetry.sqlite'
        identity = path.stat().st_ino
        with closing(sqlite3.connect(path)) as db, db:
            for column in ('service_duration', 'reader_state', 'purpose', 'error_code', 'admission',
                           'documents', 'source', 'source_collection', 'final_verification'):
                db.execute(f'ALTER TABLE events DROP COLUMN {column}')
            db.execute('PRAGMA user_version=1')
        self.start()
        upgraded = self.history(lambda h: bool(h.get('request_cohorts')))
        self.assertEqual(path.stat().st_ino, identity)
        cohort = upgraded['request_cohorts'][0]
        self.assertEqual(cohort[2:4], ['unclassified', 'unknown'])
        self.assertEqual(cohort[4:7], [1, 0, 0])
        self.assertIsNone(cohort[7])
        self.assertEqual(cohort[16:], [1, 0, 0, 0, 0, 0])

    def test_waiting_projects_do_not_block_a_ready_project(self):
        self.check_waiting_isolation(2)

    def test_waiting_limit_preserves_execution_and_control_capacity(self):
        self.check_waiting_isolation(4)

    def check_waiting_isolation(self, waiters):
        from contextlib import ExitStack
        project_count = 5 + waiters + (1 if waiters == 4 else 0)
        for i in range(project_count - 1):
            root = self.base / f'other-{i}'
            root.mkdir()
            (root / 'limits.js').write_text(f'export function readLimit() {{ return {80 + i}; }}\n')
            (root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
            self.init(root=root)
            self.roots.append(root)
        self.start(ready=False)
        deadline = time.monotonic() + 15
        while True:
            status = self.raw('service-status')['result']
            if len(status['qualified_projects']) == project_count and all(r['ready'] for r in status['resident_projects']):
                break
            self.assertLess(time.monotonic(), deadline, status)
            time.sleep(.03)
        resident = {r['root'] for r in status['resident_projects']}
        cold = [root for root in self.roots if str(root) not in resident]
        self.assertEqual(len(cold), project_count - 5)
        with ExitStack() as stack:
            clients = []
            locks = []
            for i, root in enumerate(cold[:waiters]):
                workspace = self.home / 'projects' / hashlib.sha256(str(root).encode()).hexdigest()
                lock = stack.enter_context((workspace / 'index/docs/kb/.shin-context.lock').open('rb'))
                fcntl.flock(lock, fcntl.LOCK_EX)
                locks.append(lock)
                client = stack.enter_context(self.connect())
                client.sendall((json.dumps(self.frame('query', root=root, query='readLimit', request_id=f'cold-{i}')) + '\n').encode())
                clients.append(client)
            deadline = time.monotonic() + 5
            while True:
                status = self.raw('service-status')['result']
                # Old binary counts both waiting requests as query workers.
                if status.get('waiting_requests', status['query_workers']) == waiters:
                    break
                self.assertLess(time.monotonic(), deadline, status)
                time.sleep(.02)
            if waiters == 4:
                refused = self.raw('query', root=cold[-1], query='readLimit')
                self.assertEqual(refused['error']['code'], 'service-busy')
                self.assertEqual(self.raw('service-status')['result']['waiting_requests'], 4)
            ready = next(Path(r['root']) for r in status['resident_projects'] if r['ready'])
            started = time.monotonic()
            response = self.raw('query', root=ready, query='readLimit')
            self.assertTrue(response['ok'], response)
            self.assertEqual(response['result']['root'], str(ready))
            self.assertIn((ready / 'limits.js').read_text().strip(), response['result']['source_context'])
            self.assertLess(time.monotonic() - started, 2)
            self.assertEqual(status['query_workers'], 0)
            self.assertEqual(status['max_waiting_requests'], 4)
            # Cross-project followups remain fenced even while others wait.
            denied = self.raw('read', root=ready, path='limits.js', snapshot='fnv1a64:0000000000000000')
            self.assertEqual(denied['error']['code'], 'source-snapshot-mismatch')
            for lock in locks:
                fcntl.flock(lock, fcntl.LOCK_UN)
            for root, client in zip(cold, clients):
                response = self.receive(client)
                self.assertTrue(response['ok'], response)
                self.assertEqual(response['result']['root'], str(root))
                self.assertIn((root / 'limits.js').read_text().strip(), response['result']['source_context'])

    def test_cold_reader_time_is_separate_from_worker_execution(self):
        for i in range(5):
            root = self.base / f'other-{i}'
            root.mkdir()
            (root / 'limits.js').write_text('export function readLimit() { return 7; }\n')
            (root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
            self.init(root=root)
            self.roots.append(root)
        self.start(ready=False)
        deadline = time.monotonic() + 10
        cold = None
        while time.monotonic() < deadline:
            status = self.raw('service-status')['result']
            if len(status['qualified_projects']) == 6 and all(r['ready'] for r in status['resident_projects']):
                resident = {r['root'] for r in status['resident_projects']}
                cold = next(root for root in self.roots if str(root) not in resident)
                break
            time.sleep(.03)
        self.assertIsNotNone(cold)
        response = self.raw('query', root=cold, query='readLimit', diagnostic=True)
        self.assertTrue(response['ok'], response)
        metric = self.raw('service-status')['result']['telemetry']['request_metrics']['recent'][-1]
        self.assertEqual(metric['reader_state'], 'cold')
        self.assertEqual(metric['purpose'], 'diagnostic')
        self.assertGreater(metric['service_duration_ms'], metric['duration_ms'])
        self.assertTrue(self.raw('query', root=cold, query='readLimit', diagnostic=True)['ok'])
        metric = self.raw('service-status')['result']['telemetry']['request_metrics']['recent'][-1]
        self.assertEqual(metric['reader_state'], 'warm')



    def wait_service_state(self, predicate, timeout=5):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.raw('service-status')['result']
            if predicate(state):return state
            time.sleep(.02)
        self.fail('service state did not converge: '+json.dumps(state))

    def hold_reader(self, root=None):
        root=root or self.root
        readers=self.raw('service-status')['result']['resident_projects']
        pid=next(row['pid'] for row in readers if row['root']==str(root) and row['ready'])
        os.kill(pid,signal.SIGSTOP)
        def release():
            try:os.kill(pid,signal.SIGCONT)
            except ProcessLookupError:pass
        self.addCleanup(release)
        return pid

    def test_first_query_during_explicit_service_start_has_a_bound_reader(self):
        # Hold qualification until the first client is waiting, so constructor
        # admission cannot race ahead of the test's startup query.
        with (self.workspace()/'index/docs/kb/.shin-context.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.start(ready=False)
            pending = self.connect()
            pending.sendall((json.dumps(self.frame('query', query='readLimit', request_id='first-startup-query'))+'\n').encode())
            self.wait_service_state(lambda state: state['waiting_requests'] == 1)
            fcntl.flock(lock, fcntl.LOCK_UN)
        result = self.receive(pending)
        self.assertTrue(result['ok'], result)
        self.assertIn('return 7;', result['result']['source_context'])
        status = self.raw('service-status')['result']
        self.assertEqual(status['resident_errors'], {})
        self.assertTrue(self.raw('query', query='readLimit')['ok'])

    def test_short_same_project_collision_waits_and_preserves_execution_capacity(self):
        other=self.base/'independent';other.mkdir()
        (other/'limits.js').write_text('export function readLimit() { return 99; }\n')
        self.init(root=other);self.roots.append(other);self.start()
        pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='first'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        second=self.connect();second.sendall((json.dumps(self.frame('query',query='readLimit',request_id='second'))+'\n').encode())
        waiting=self.wait_service_state(lambda state:state['waiting_requests']==1)
        # A queued request has not consumed the second execution slot.
        self.assertEqual(waiting['query_workers'],1)
        # Release immediately: completing an unrelated request first used to
        # spend this waiter's real 500 ms execution-admission deadline.
        os.kill(pid,signal.SIGCONT)
        original=self.receive(first);self.assertTrue(original['ok'],original)
        response=self.receive(second);self.assertTrue(response['ok'],response)
        self.assertEqual(original['result']['snapshot_id'],response['result']['snapshot_id'])
        metrics=self.raw('service-status')['result']['telemetry']['request_metrics']['recent']
        queued=next(row for row in metrics if row['request_id']==hashlib.sha256(b'second').hexdigest())
        self.assertGreater(queued['wait_ms'],0)
        self.assertEqual(queued['reader_state'],'warm')
        self.assertEqual(queued['stage'],'execution')

    def test_busy_project_waiter_does_not_block_an_independent_project(self):
        other=self.base/'independent';other.mkdir()
        (other/'limits.js').write_text('export function readLimit() { return 99; }\n')
        self.init(root=other);self.roots.append(other);self.start()
        pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='holding'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        second=self.connect();second.sendall((json.dumps(self.frame('query',query='readLimit',request_id='busy-waiter'))+'\n').encode())
        self.wait_service_state(lambda state:state['waiting_requests']==1)
        # The first reader remains stopped throughout the independent query.
        # Its waiter must expire without execution, even on a slow CI worker.
        began=time.monotonic();independent=self.raw('query',root=other,query='readLimit')
        self.assertTrue(independent['ok'],independent);self.assertLess(time.monotonic()-began,2)
        self.assertIn('return 99;',independent['result']['source_context'])
        response=self.receive(second)
        self.assertEqual(response['error']['code'],'service-busy',response)
        self.assertEqual(response['error']['diagnostic']['execution_started'],False)
        self.assertEqual(response['error']['diagnostic']['reason'],'project-executing')
        self.assertEqual(self.raw('service-status')['result']['query_workers'],1)
        os.kill(pid,signal.SIGCONT);self.assertTrue(self.receive(first)['ok'])
        self.assertTrue(self.raw('query',query='readLimit')['ok'])

    def test_queued_busy_request_expires_once_without_execution(self):
        self.start();pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='holding'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        second=self.connect();started=time.monotonic()
        second.sendall((json.dumps(self.frame('query',query='readLimit',request_id='busy-timeout'))+'\n').encode())
        response=self.receive(second);elapsed=time.monotonic()-started
        self.assertEqual(response['error']['code'],'service-busy',response)
        self.assertGreaterEqual(elapsed,.45);self.assertLess(elapsed,2)
        self.assertEqual(response['error']['diagnostic']['execution_started'],False)
        self.assertEqual(response['error']['diagnostic']['reason'],'project-executing')
        state=self.raw('service-status')['result']
        self.assertEqual(state['waiting_requests'],0);self.assertEqual(state['query_workers'],1)
        metric=next(row for row in state['telemetry']['request_metrics']['recent'] if row['request_id']==hashlib.sha256(b'busy-timeout').hexdigest())
        self.assertEqual(metric['stage'],'admission')
        os.kill(pid,signal.SIGCONT);self.assertTrue(self.receive(first)['ok'])
        self.assertTrue(self.raw('query',query='readLimit')['ok'])

    def test_project_queue_is_fifo_bounded_and_resolves_each_frame_once(self):
        service=self.start();pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='serial-0'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        # Queue an ordered batch before admission starts. Serial status round
        # trips used to spend the real 500 ms deadline on test coordination.
        # The reader stays stopped; all queue/capacity observations are genuine.
        os.kill(service.pid,signal.SIGSTOP)
        try:
            queued=[]
            for i in range(1,5):
                client=self.connect();queued.append(client)
                client.sendall((json.dumps(self.frame('query',query='readLimit',request_id=f'serial-{i}'))+'\n').encode())
            overflow=self.connect();overflow.sendall((json.dumps(self.frame('query',query='readLimit',request_id='over-capacity'))+'\n').encode())
            status_client=self.connect();status_client.sendall((json.dumps(self.frame('status',wait_for_ready=False))+'\n').encode())
            probe=self.connect();probe.sendall((json.dumps(self.frame('service-status'))+'\n').encode())
        finally:
            began=time.monotonic();os.kill(service.pid,signal.SIGCONT)
        refused=self.receive(overflow);overflow.close()
        status=self.receive(status_client);status_client.close()
        state=self.receive(probe)['result'];probe.close()
        os.kill(pid,signal.SIGCONT)
        self.assertLess(time.monotonic()-began,.5)
        self.assertEqual(refused['error']['code'],'service-busy')
        self.assertEqual(refused['error']['diagnostic']['reason'],'waiting-capacity')
        self.assertEqual(state['max_waiting_requests'],4);self.assertEqual(state['max_query_workers'],2)
        self.assertEqual(state['waiting_requests'],4);self.assertEqual(state['query_workers'],1)
        self.assertEqual(status['error']['code'],'service-busy')
        results=[self.receive(first),*(self.receive(client) for client in queued)]
        final_state=self.raw('service-status')['result']
        self.assertEqual(final_state['waiting_requests'],0)
        self.assertEqual(final_state['query_workers'],0)
        rows=final_state['telemetry']['request_metrics']['recent']
        expected=[hashlib.sha256(f'serial-{i}'.encode()).hexdigest() for i in range(5)]
        decisions=[row for row in rows if row['request_id'] in expected]
        print('fifo_decisions='+json.dumps([{key:row.get(key) for key in (
            'request_id','stage','error_code','wait_ms','duration_ms','service_duration_ms')}
            for row in decisions]),flush=True)
        self.assertTrue(results[0]['ok'],results[0])
        self.assertCountEqual([row['request_id'] for row in decisions],expected)
        completed=[]
        for i,result in enumerate(results):
            self.assertEqual(result['request_id'],f'serial-{i}')
            row=next(row for row in decisions if row['request_id']==expected[i])
            if result['ok']:
                self.assertEqual(row['stage'],'execution')
                self.assertIn('return 7;',result['result']['source_context'])
                completed.append(expected[i])
            else:
                # Admission to the bounded queue is not an execution promise.
                # Slower hosts can exhaust 500 ms while earlier frames execute.
                # Such frames must be refused exactly once, without execution.
                self.assertEqual(result['error']['code'],'service-busy',result)
                diagnostic=result['error']['diagnostic']
                self.assertEqual(diagnostic['reason'],'project-executing')
                self.assertFalse(diagnostic['execution_started'])
                self.assertEqual(diagnostic['execution_wait_limit_ms'],500)
                self.assertEqual(row['stage'],'admission')
                self.assertEqual(row['error_code'],'service-busy')
                self.assertGreaterEqual(row['wait_ms'],500)
        executed=[row['request_id'] for row in decisions if row['stage']=='execution']
        self.assertEqual(executed,completed)

    def test_cancelled_waiter_releases_only_its_queue_slot(self):
        self.start();pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='active-peer'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        cancelled=self.connect();cancelled.sendall((json.dumps(self.frame('query',query='readLimit',request_id='cancelled-peer'))+'\n').encode())
        self.wait_service_state(lambda state:state['waiting_requests']==1)
        cancelled.close();self.wait_service_state(lambda state:state['waiting_requests']==0)
        self.assertEqual(self.raw('service-status')['result']['query_workers'],1)
        replacement=self.connect();replacement.sendall((json.dumps(self.frame('query',query='readLimit',request_id='replacement-peer'))+'\n').encode())
        self.wait_service_state(lambda state:state['waiting_requests']==1)
        os.kill(pid,signal.SIGCONT)
        self.assertTrue(self.receive(first)['ok']);self.assertTrue(self.receive(replacement)['ok'])
        rows=self.raw('service-status')['result']['telemetry']['request_metrics']['recent']
        cancelled=next(row for row in rows if row['request_id']==hashlib.sha256(b'cancelled-peer').hexdigest())
        self.assertEqual(cancelled['stage'],'admission');self.assertEqual(cancelled['error_code'],'client-disconnected')

    def test_active_peer_disconnect_does_not_cancel_waiting_request(self):
        self.start();self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='disconnecting'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        queued=self.connect();queued.sendall((json.dumps(self.frame('query',query='readLimit',request_id='surviving'))+'\n').encode())
        self.wait_service_state(lambda state:state['waiting_requests']==1)
        first.close();response=self.receive(queued)
        # A stopped worker may need forced termination; the original bounded
        # wait may then expire, but another caller's cancellation is not ours.
        if not response['ok']:self.assertEqual(response['error']['code'],'project-updating',response)
        self.wait_service_state(lambda state:state['waiting_requests']==0 and state['query_workers']==0)
        self.assertTrue(self.raw('query',query='readLimit')['ok'])

    def test_queued_read_never_bypasses_snapshot_freshness(self):
        self.start();snapshot=self.raw('query',query='readLimit')['result']['snapshot_id']
        pid=self.hold_reader()
        first=self.connect();first.sendall((json.dumps(self.frame('query',query='readLimit',request_id='before-edit'))+'\n').encode())
        self.wait_service_state(lambda state:state['query_workers']==1)
        queued=self.connect();queued.sendall((json.dumps(self.frame('read',path='limits.js',snapshot=snapshot,request_id='after-edit'))+'\n').encode())
        self.wait_service_state(lambda state:state['waiting_requests']==1)
        (self.root/'limits.js').write_text('export function readLimit() { return 8; }\n')
        try:os.kill(pid,signal.SIGCONT)
        except ProcessLookupError:pass
        response=self.receive(queued)
        self.assertFalse(response['ok'],response)
        self.assertNotIn('return 7;',json.dumps(response))
        self.assertIn(response['error']['code'],('project-updating','service-busy','snapshot-mismatch','query-snapshot-changed','query-source-changed'))
        deadline=time.monotonic()+10
        while True:
            current=self.raw('query',query='readLimit')
            if current['ok'] and 'return 8;' in current['result'].get('source_context',''):break
            self.assertLess(time.monotonic(),deadline,current);time.sleep(.05)

    def test_slow_input_and_saturation_are_bounded(self):
        child = self.start()
        slow = self.connect()
        slow.sendall(b'{')
        self.assertTrue(self.raw('service-status')['ok'])
        time.sleep(2.2)
        self.assertEqual(slow.recv(1), b'')
        # Queue a complete batch while the supervisor is paused, so worker
        # admission is deterministic rather than relying on corpus duration.
        child.send_signal(signal.SIGSTOP)
        clients = [self.connect() for _ in range(6)]
        for client in clients:
            client.sendall((json.dumps(self.frame('query', query='readLimit')) + '\n').encode())
        child.send_signal(signal.SIGCONT)
        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(pool.map(self.receive, clients))
        self.assertTrue(all(r.get('ok') or r.get('error',{}).get('code')=='service-busy' for r in results),results)
        self.assertGreaterEqual(sum(r.get('ok',False) for r in results),1)
        deadline = time.monotonic() + 5
        while self.raw('service-status')['result']['query_workers'] and time.monotonic() < deadline:
            time.sleep(0.03)
        self.assertEqual(self.raw('service-status')['result']['query_workers'], 0)
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])

    def history(self, predicate=lambda h: h.get('status') == 'ready'):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            result = self.raw('service-status')['result']['telemetry']['history']
            if predicate(result):
                return result
            time.sleep(0.05)
        self.fail('history condition not reached: ' + json.dumps(result))

    def test_history_restart_privacy_retention_and_schema_refusal(self):
        child = self.start()
        self.run_ce('query', 'readLimit PRIVATE-QUERY-MARKER', '--service')
        first = self.history(lambda h: h.get('status') == 'ready' and h.get('projects'))
        self.assertEqual(first['projects'][0]['requests'], 1)
        self.assertTrue(first['minute_timeline'])
        self.assertLessEqual(len(first['minute_timeline']), 61)
        self.assertTrue(all(len(row) == 11 for row in first['minute_timeline']))
        self.assertEqual(sum(row[1] for row in first['minute_timeline']), 1)
        self.assertEqual(first['dropped_current_run'], 0)
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        path = self.endpoint.parent / 'history/telemetry.sqlite'
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertLess(path.stat().st_size, 32 * 1024 * 1024)
        self.assertNotIn(b'PRIVATE-QUERY-MARKER', path.read_bytes())
        self.assertNotIn(str(self.root).encode(), path.read_bytes())
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("INSERT INTO events(run,at_ms,kind) VALUES('old',1,'sample')")
            db.executemany("INSERT INTO events(run,at_ms,kind) VALUES('cap',?,'sample')",
                           [(int(time.time() * 1000),)] * 20001)
        child = self.start()
        retained = self.history()
        self.assertLessEqual(retained['rows'], 20000)
        self.assertGreater(retained['pruned_current_run'], 0)
        self.assertEqual(retained['projects'][0]['requests'], 1)
        self.assertEqual(retained['row_limits']['sample'], 3000)
        self.assertEqual(retained['row_limits']['refresh_success'], 1000)
        self.assertEqual(retained['row_limits']['refresh_error'], 1000)
        self.assertLessEqual(len(retained['timeline']), 169)
        self.run_ce('query', 'readLimit', '--service')
        self.history(lambda h: h.get('status') == 'ready' and h.get('projects'))
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        child = self.start()
        restored = self.history()
        self.assertEqual(restored['projects'][0]['requests'], 2)
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        with closing(sqlite3.connect(path)) as db, db:
            db.execute('PRAGMA user_version=999')
        original = path.read_bytes()
        self.start()
        rejected = self.history(lambda h: h.get('status') == 'unavailable')
        self.assertEqual(rejected['error_code'], 'history-schema-unsupported')
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])
        self.assertEqual(path.read_bytes(), original)

    def test_large_history_status_is_drained_and_stop_ack_stays_small(self):
        for i in range(3):
            root = self.base / (f'other-{i}-' + 'long-name-' * 20)
            root.mkdir()
            (root / 'limits.js').write_text('export function readLimit() { return 7; }\n')
            (root / 'README.md').write_text('# Limits\n\nreadLimit owns the ceiling.\n')
            self.init(root=root)
            self.roots.append(root)
        child = self.start()
        self.history()
        self.run_ce('service-stop')
        child.communicate(timeout=15)
        now = int(time.time() * 1000)
        with closing(sqlite3.connect(self.endpoint.parent / 'history/telemetry.sqlite')) as db, db:
            db.executemany("INSERT INTO events(run,at_ms,kind,rss,workers) VALUES(?,?,'sample',123456789,2)",
                           [('r' * 64, now - i * 3600000) for i in range(168)] +
                           [('r' * 64, now - i * 60000) for i in range(60)])
            db.executemany("INSERT INTO events(run,at_ms,kind,project,method,purpose,reader_state,outcome,duration,service_duration,"
                           "admission,documents,source,source_collection,final_verification) "
                           "VALUES('cohort',?,'request',?,'query',?,?,'response-ready',10.123456789012345,20.123456789012345,"
                           "1.123456789012345,2.123456789012345,3.123456789012345,4.123456789012345,5.123456789012345)",
                           [(now, hashlib.sha256(str(i).encode()).hexdigest(), purpose, reader)
                            for i in range(16) for purpose in ('diagnostic', 'unclassified') for reader in ('cold', 'warm')])
        child = self.start()
        self.history(lambda h: len(h.get('timeline', [])) >= 167 and len(h.get('minute_timeline', [])) >= 59)
        self.assertTrue(self.run_ce('service-status')['ok'])
        bounded = self.history(lambda h: h.get('request_cohorts_truncated') is True)
        self.assertEqual(len(bounded['request_cohorts']), 48)
        for _ in range(64):
            self.run_ce('query', 'readLimit', '--service')
        status = self.run_ce('service-status')
        self.assertLessEqual(len(json.dumps(status, separators=(',', ':')).encode()) + 1, 65536)
        telemetry = status['telemetry']
        self.assertTrue(telemetry['projection']['truncated'])
        self.assertGreater(telemetry['projection']['omitted_rows']['/request_metrics/recent'], 0)
        self.assertEqual(telemetry['request_metrics']['responses_ready'], 64)
        self.assertEqual(len(telemetry['project_states']), 4)
        self.assertGreaterEqual(len(telemetry['history']['timeline']), 167)
        omitted = telemetry['projection']['omitted_rows'].get('/history/request_cohorts', 0)
        self.assertEqual(len(telemetry['history']['request_cohorts']) + omitted, 48)
        if omitted:
            self.assertTrue(telemetry['history']['request_cohorts_truncated'])
        self.assertGreater(len(telemetry['samples']), 0)
        self.assertGreaterEqual(len(telemetry['request_metrics']['recent']), 16)
        with self.connect() as client:
            client.sendall((json.dumps(self.frame('service-status')) + '\n').encode())
            # Expose partial writes instead of continuously emptying the receive queue.
            time.sleep(.15)
            response = self.receive(client)
        self.assertGreater(len(json.dumps(response)), 16000)
        response = self.raw('service-stop')
        self.assertTrue(response['result']['stop_requested'])
        self.assertNotIn('telemetry', response['result'])
        self.assertLess(len(json.dumps(response)), 8192)
        output, _ = child.communicate(timeout=15)
        self.assertEqual(json.loads(output)['undelivered_stop_replies'], 0)
        self.assertEqual(child.returncode, 0)
        with closing(sqlite3.connect(self.endpoint.parent / 'history/telemetry.sqlite')) as db, db:
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE run='cohort'").fetchone()[0], 64)
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='request' AND run!='cohort'").fetchone()[0], 64)
        child = self.start()
        self.history(lambda h: len(h.get('timeline', [])) >= 167)
        with self.connect() as client:
            client.sendall((json.dumps(self.frame('service-stop')) + '\n').encode())
            output, _ = child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0, 'non-reading caller held shutdown open')
        self.assertEqual(json.loads(output)['undelivered_stop_replies'], 0)

    def test_history_symlink_does_not_block_queries(self):
        self.endpoint.parent.mkdir(mode=0o700, exist_ok=True)
        target = self.base / 'foreign'
        target.mkdir(mode=0o700)
        (self.endpoint.parent / 'history').symlink_to(target)
        self.start()
        self.history(lambda h: h.get('status') == 'unavailable')
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])
        self.assertEqual(list(target.iterdir()), [])

    def test_duplicate_restart_and_no_silent_fallback(self):
        child = self.start()
        duplicate = subprocess.run([str(fixtures.TOOL), 'serve', '--root', str(self.root), '--home', str(self.home)],
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(duplicate.returncode, 2)
        self.assertEqual(json.loads(duplicate.stderr)['error']['code'], 'writer-busy')
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])
        child.kill()
        child.communicate(timeout=10)
        self.assertTrue(self.endpoint.exists())
        self.run_ce('query', 'readLimit', '--service', expected=2)
        self.assertTrue(self.run_ce('query', 'readLimit')['ok'])
        self.start()
        self.assertTrue(self.run_ce('query', 'readLimit', '--service')['ok'])

    def test_foreign_endpoint_not_removed(self):
        directory = self.endpoint.parent
        directory.mkdir(mode=0o700)
        self.endpoint.write_text('preserve')
        self.endpoint.chmod(0o600)
        run = subprocess.run([str(fixtures.TOOL), 'serve', '--root', str(self.root), '--home', str(self.home)],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 2)
        self.assertEqual(json.loads(run.stderr)['error']['code'], 'service-socket-unsafe')
        self.assertEqual(self.endpoint.read_text(), 'preserve')

    def test_endpoint_replacement_stops_without_deleting_replacement(self):
        child = self.start()
        self.endpoint.unlink()
        self.endpoint.write_text('not-our-endpoint')
        self.endpoint.chmod(0o600)
        stdout, stderr = child.communicate(timeout=10)
        self.assertEqual(child.returncode, 2, stdout + stderr)
        self.assertEqual(self.endpoint.read_text(), 'not-our-endpoint')

    def test_executable_replacement_requires_restart(self):
        tool = self.base / 'shin-ce'
        shutil.copy2(fixtures.TOOL, tool)
        self.start(tool)
        replacement = self.base / 'replacement'
        shutil.copy2(fixtures.TOOL, replacement)
        replacement.replace(tool)
        result = self.raw('query', query='readLimit')
        self.assertEqual(result['error']['code'], 'service-executable-changed')
        self.assertTrue(self.raw('service-status')['ok'])


if __name__ == '__main__':
    class TimedResult(unittest.TextTestResult):
        def startTest(self,test):
            self.started=time.monotonic();super().startTest(test)
        def stopTest(self,test):
            self.stream.writeln(f'  case_seconds={time.monotonic()-self.started:.3f} {test.id()}')
            super().stopTest(test)
    runner=unittest.TextTestRunner(verbosity=2,resultclass=TimedResult)
    import sys
    if '--group' in sys.argv:
        at=sys.argv.index('--group');group=sys.argv[at+1];del sys.argv[at:at+2]
        dedicated={'refresh-deadline':{'test_updating_wait_is_bounded_and_disconnect_releases_capacity'},
                   'fifo':{'test_project_queue_is_fifo_bounded_and_resolves_each_frame_once'}}
        names=set(unittest.defaultTestLoader.getTestCaseNames(LocalServiceTests))
        assert set.union(*dedicated.values()) <= names
        groups={**dedicated,'ordinary':names-set.union(*dedicated.values())}
        assert set.union(*groups.values())==names and sum(map(len,groups.values()))==len(names)
        assert group in groups and len(sys.argv)==1
        result=runner.run(unittest.TestSuite(LocalServiceTests(name) for name in sorted(groups[group])))
        sys.exit(not result.wasSuccessful())
    unittest.main(testRunner=runner)
