#!/usr/bin/env python3
"""Protocol-to-service native tests, with no model inference or Python SDK."""
import json
import hashlib
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time
import threading
import unittest

MCP = Path(sys.argv.pop(1)).resolve()
import local_service_test as service
from integration_test import Fixture


class McpTests(unittest.TestCase):
    setUp = service.LocalServiceTests.setUp
    init = service.LocalServiceTests.init
    run_ce = service.LocalServiceTests.run_ce
    start = service.LocalServiceTests.start
    raw = service.LocalServiceTests.raw
    frame = service.LocalServiceTests.frame
    connect = service.LocalServiceTests.connect
    receive = service.LocalServiceTests.receive

    def adapter(self, root=None, roots=None):
        argv = [str(MCP), '--home', str(self.home)]
        for selected in roots or [root or self.root]:
            argv += ['--root', str(selected)]
        child = subprocess.Popen(argv,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        child.sequence = 0
        child.pending = b''
        child.responses = {}
        def cleanup():
            if child.poll() is None:
                child.terminate()
            try:
                child.communicate(timeout=40)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()
                self.fail('MCP adapter did not stop')
        self.addCleanup(cleanup)
        response = self.rpc(child, 'initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'shin-native-test', 'version': '1'}})
        self.assertEqual(response['result']['protocolVersion'], '2025-11-25')
        self.assertEqual(response['result']['serverInfo']['name'], 'shin')
        child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        child.stdin.flush()
        return child

    def rpc(self, child, method, params, timeout=15):
        request_id = self.send(child, method, params)
        return self.response(child, request_id, timeout=timeout)

    def send(self, child, method, params):
        child.sequence += 1
        request = {'jsonrpc': '2.0', 'id': child.sequence, 'method': method, 'params': params}
        child.stdin.write((json.dumps(request) + '\n').encode())
        child.stdin.flush()
        return child.sequence

    def response(self, child, request_id, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if request_id in child.responses:
                return child.responses.pop(request_id)
            while b'\n' in child.pending:
                line, child.pending = child.pending.split(b'\n', 1)
                value = json.loads(line)
                if value.get('id') == request_id:
                    return value
                if 'id' in value:
                    child.responses[value['id']] = value
            readable, _, _ = select.select([child.stdout], [], [], max(0, deadline - time.monotonic()))
            if readable:
                part = os.read(child.stdout.fileno(), 8192)
                self.assertTrue(part, 'MCP stdout ended without a response')
                child.pending += part
        self.fail('MCP response timeout')

    def notify_cancel(self, child, request_id):
        child.stdin.write((json.dumps({'jsonrpc': '2.0', 'method': 'notifications/cancelled',
            'params': {'requestId': request_id}}) + '\n').encode())
        child.stdin.flush()

    def delayed_service(self):
        self.endpoint.parent.mkdir(mode=0o700)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(str(self.endpoint))
        self.endpoint.chmod(0o600)
        listener.listen(4)
        listener.settimeout(3)
        return listener

    def pending_query(self, child, listener):
        request_id = self.send(child, 'tools/call', {'name': 'shin_query', 'arguments': {'query': 'readLimit'}})
        connection, _ = listener.accept()
        self.addCleanup(connection.close)
        connection.settimeout(2)
        data = b''
        while not data.endswith(b'\n'):
            data += connection.recv(8192)
        self.assertEqual(json.loads(data)['command'], 'query')
        return request_id, connection

    def tool(self, child, name, arguments, timeout=15):
        result = self.rpc(child, 'tools/call', {'name': name, 'arguments': arguments}, timeout=timeout)['result']
        native = json.loads(result['content'][0]['text'])
        self.assertEqual(result.get('isError', False), not native['ok'])
        return native

    def test_quarantined_file_keeps_mcp_available_with_visible_coverage(self):
        secret = 'sk-' + 'Q7x9' * 16
        (self.root / 'unsafe.js').write_text('export const token = "' + secret + '"; // gitleaks:allow\n')
        self.run_ce('watch', '--samples', '3')
        self.start()
        child = self.adapter()
        query = self.tool(child, 'shin_query', {'query': 'readLimit'})
        self.assertTrue(query['ok'])
        self.assertEqual(query['source_coverage']['state'], 'partial')
        self.assertEqual(query['source_coverage']['quarantined_files'], 1)
        self.assertIn('return 7;', query['source_context'])
        self.assertNotIn(secret, json.dumps(query))
        redacted = self.tool(child, 'shin_read', {'path': 'unsafe.js', 'snapshot': query['snapshot_id']})
        self.assertTrue(redacted['ok'])
        self.assertIn('redacted', redacted['source_context'])
        self.assertNotIn(secret, json.dumps(redacted))
        status = self.tool(child, 'shin_status', {})
        self.assertEqual(status['source_coverage']['quarantined_files'], 1)
        service_status = self.raw('service-status')['result']
        project = next(p for p in service_status['telemetry']['project_states'] if p['root'] == str(self.root))
        self.assertEqual(project['source_coverage']['quarantined_files'], 1)

    def test_explicit_chat_identity_and_query_follow_up_are_private(self):
        self.start()
        child = self.adapter()
        args = {'query': 'readLimit', 'session_id': 'PRIVATE-CHAT-123'}
        query = self.tool(child, 'shin_query', args)
        trace = query['request_trace']
        self.assertTrue(trace['session_correlated'])
        self.assertEqual(len(trace['request_id']), 64)
        result = self.tool(child, 'shin_read', {'path': 'limits.js', 'snapshot': query['snapshot_id'],
            'session_id': args['session_id'], 'parent_request_id': trace['request_id']})
        self.assertTrue(result['ok'], result)
        metrics = self.raw('service-status')['result']['telemetry']['request_metrics']
        self.assertEqual(metrics['recent'][-1]['follow_up'], 'observed')
        self.assertEqual(metrics['recent'][-1]['session_id'], metrics['recent'][-2]['session_id'])
        self.assertNotIn('PRIVATE-CHAT', json.dumps(metrics))
        self.assertEqual(metrics['task_outcome'], 'not-observed')
        bad = self.tool(child, 'shin_read', {'path': 'limits.js', 'snapshot': query['snapshot_id'],
            'session_id': args['session_id'], 'parent_request_id': 'x' * 64})
        self.assertFalse(bad['ok'])

    def test_catalog_query_inspect_read_and_eof(self):
        self.start()
        child = self.adapter()
        tools = self.rpc(child, 'tools/list', {})['result']['tools']
        self.assertEqual({t['name'] for t in tools}, {'shin_query', 'shin_inspect', 'shin_read', 'shin_status', 'shin_help'})
        for tool in tools:
            self.assertTrue(tool['annotations']['readOnlyHint'])
            self.assertFalse(tool['annotations']['destructiveHint'])
            self.assertFalse(tool['inputSchema']['additionalProperties'])
        query = self.tool(child, 'shin_query', {'query': 'readLimit'})
        self.assertIn('return 7;', query['source_context'])
        self.assertEqual(query['transport'], 'local-socket-v1')
        args = {'path': 'limits.js', 'snapshot': query['snapshot_id']}
        self.assertTrue(self.tool(child, 'shin_inspect', args)['ok'])
        self.assertTrue(self.tool(child, 'shin_read', args)['ok'])
        self.assertTrue(self.tool(child, 'shin_status', {})['ok'])
        child.stdin.close()
        child.stdin = None
        child.wait(timeout=10)
        self.assertEqual(child.returncode, 0)

    def test_project_binding_cannot_be_overridden(self):
        other = self.base / 'other'
        other.mkdir()
        (other / 'README.md').write_text('# Other\n')
        (other / 'limits.js').write_text('export function readLimit() { return 99; }\n')
        self.init(root=other)
        self.roots.append(other)
        self.start()
        left = self.adapter()
        right = self.adapter(other)
        self.assertFalse(self.tool(left, 'shin_query', {'query': 'readLimit', 'root': str(other)})['ok'])
        self.assertIn('return 7;', self.tool(left, 'shin_query', {'query': 'readLimit'})['source_context'])
        self.assertIn('return 99;', self.tool(right, 'shin_query', {'query': 'readLimit'})['source_context'])
        foreign = self.tool(right, 'shin_query', {'query': 'readLimit'})['snapshot_id']
        self.assertFalse(self.tool(left, 'shin_read', {'path': 'limits.js', 'snapshot': foreign})['ok'])
        denied = self.rpc(left, 'tools/call', {'name': 'service-stop', 'arguments': {}})
        self.assertTrue('error' in denied or denied.get('result', {}).get('isError'))
        self.assertTrue(self.raw('service-status')['ok'])

    def test_static_help_and_service_failure_guidance(self):
        listener = self.delayed_service()
        before = {str(p.relative_to(self.home)): p.read_bytes()
                  for p in self.home.rglob('*') if p.is_file()}
        child = self.adapter()
        help_result = self.tool(child, 'shin_help', {})
        self.assertEqual(help_result['schema_version'], 'shin.mcp-help.v1')
        self.assertEqual(help_result['scope'], 'static-guidance')
        self.assertFalse(help_result['readiness_assessed'])
        self.assertEqual(help_result['versions']['observer'], self.identity['version'])
        self.assertEqual(help_result['versions']['core'], self.identity['core_version'])
        self.assertLess(len(json.dumps(help_result)), 16384)
        self.assertNotIn(str(self.root), json.dumps(help_result))
        self.assertEqual(select.select([listener], [], [], 0)[0], [])
        for args in ({'error_code': '../private'}, {'error_code': 'x' * 97},
                     {'error_code': 7}, {'root': str(self.root)}, {'session_id': 'not-a-project-request'}):
            invalid = self.tool(child, 'shin_help', args)
            self.assertEqual(invalid['error']['code'], 'invalid-arguments')
        diagnostic = {'schema_version': 'shin.validation-diagnostic.v1', 'truncated': False,
                      'gates': [{'id': 'eval', 'cases': [{'id': 'expected-guide',
                          'reasons': ['missing-required-path'], 'expected_paths': ['docs/guide.md']}]}]}
        for code, kind in [('source-observation-failed', 'maintainer-diagnosis'),
                           ('unknown-future-code', 'maintainer-diagnosis'),
                           ('refresh-validation-failed', 'project-check'),
                           ('storage-full', 'environment-action'),
                           ('access-denied', 'environment-action'),
                           ('source-io-error', 'environment-action')]:
            request_id = self.send(child, 'tools/call', {'name': 'shin_query',
                                                        'arguments': {'query': 'readLimit'}})
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                data = b''
                while not data.endswith(b'\n'):
                    data += connection.recv(8192)
                request = json.loads(data)
                reply = {k: request[k] for k in ('schema_version', 'request_id', 'observer_version', 'core_version')}
                error = {'code': code, 'message': 'Test service refusal.'}
                if code == 'refresh-validation-failed':
                    error['diagnostic'] = diagnostic
                if code in ('access-denied', 'source-io-error'):
                    error['diagnostic'] = {'schema_version': 'shin.source-observation-diagnostic.v1',
                        'stage': 'source-inventory', 'operation': 'read-source-content',
                        'category': 'access-denied' if code == 'access-denied' else 'io-error'}
                reply.update(ok=False, error=error)
                connection.sendall((json.dumps(reply) + '\n').encode())
                response = self.response(child, request_id)['result']
            self.assertTrue(response['isError'])
            native = json.loads(response['content'][0]['text'])
            self.assertEqual(native['error'], error)
            hint = native['recovery']
            self.assertEqual(hint['kind'], kind)
            if code in ('access-denied', 'source-io-error'):
                self.assertEqual(hint['diagnostic'], error['diagnostic'])
            self.assertNotIn('retry_after_ms', hint)
            self.assertNotIn('source_context', native)
            if kind == 'maintainer-diagnosis':
                self.assertEqual(hint['cause_status'], 'unconfirmed')
                self.assertEqual(hint['responsible_party'], 'shin-maintainers')
                self.assertNotIn('next_tool', hint)
        self.assertEqual(select.select([listener], [], [], 0)[0], [])
        after = {str(p.relative_to(self.home)): p.read_bytes()
                 for p in self.home.rglob('*') if p.is_file()}
        self.assertEqual(before, after)

    def test_help_does_not_bypass_changed_project_binding(self):
        child = self.adapter()
        self.root.rename(self.base / 'moved-project')
        help_result = self.tool(child, 'shin_help', {'error_code': 'source-observation-failed'})
        self.assertTrue(help_result['ok'])
        self.assertEqual(help_result['scope'], 'static-guidance')
        refused = self.tool(child, 'shin_status', {})
        self.assertFalse(refused['ok'])
        self.assertNotIn('source_context', refused)
        self.assertFalse(self.endpoint.exists())

    def test_empty_startup_root_is_never_implicit_cwd(self):
        for roots in ([''], ['', str(self.root)], [str(self.root), '']):
            argv = [str(MCP), '--home', str(self.home)]
            for root in roots:
                argv += ['--root', root]
            result = subprocess.run(argv, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(result.stdout)

    def test_multi_project_navigation_parallel_and_scoped_followup(self):
        other = self.base / 'nested' / self.root.name
        other.mkdir(parents=True)
        (other / 'README.md').write_text('# Other\n')
        (other / 'limits.js').write_text('export function readLimit() { return 99; }\n')
        hidden = self.base / 'not-allowed'
        hidden.mkdir()
        (hidden / 'README.md').write_text('# Private project\n')
        (hidden / 'limits.js').write_text('export function readLimit() { return 123; }\n')
        for root in (other, hidden):
            self.init(root=root)
            self.roots.append(root)
        self.start()
        child = self.adapter(roots=[self.root, other])
        tools = self.rpc(child, 'tools/list', {})['result']['tools']
        self.assertEqual(len(tools), 6)
        for tool in tools:
            self.assertTrue(tool['annotations']['readOnlyHint'])
            if tool['name'] not in ('shin_projects', 'shin_help'):
                self.assertIn('project', tool['inputSchema']['required'])
        help_result = self.tool(child, 'shin_help', {})
        self.assertEqual(help_result['scope'], 'static-guidance')
        self.assertNotIn('mcp_project', help_result)
        self.assertFalse(self.tool(child, 'shin_help', {'project': 'not-an-authority'})['ok'])
        catalog = self.tool(child, 'shin_projects', {})
        self.assertFalse(catalog['readiness_assessed'])
        self.assertEqual({r['root'] for r in catalog['projects']}, {str(self.root), str(other)})
        self.assertEqual(catalog['projects'][0]['name'], catalog['projects'][1]['name'])
        handles = {r['root']: r['project'] for r in catalog['projects']}
        a, b = handles[str(self.root)], handles[str(other)]
        partial = self.adapter(roots=[self.base / 'missing-at-startup', other])
        partial_catalog = self.tool(partial, 'shin_projects', {})['projects']
        self.assertFalse(partial_catalog[0]['binding_active'])
        self.assertTrue(partial_catalog[1]['binding_active'])
        self.assertEqual(self.tool(partial, 'shin_status', {'project': partial_catalog[1]['project']})['root'], str(other))
        privileged = self.adapter(roots=[self.root, hidden])
        third = next(r['project'] for r in self.tool(privileged, 'shin_projects', {})['projects'] if r['root'] == str(hidden))
        self.assertEqual(self.tool(child, 'shin_status', {'project': third})['error']['code'], 'mcp-project-denied')
        for args in ({}, {'project': str(hidden)}, {'project': a, 'root': str(hidden)}):
            self.assertFalse(self.tool(child, 'shin_status', args)['ok'])
        # Keep two calls outstanding; returning B before reading A must not
        # change A's selected root or provenance.
        left = self.send(child, 'tools/call', {'name': 'shin_query', 'arguments': {'project': a, 'query': 'readLimit'}})
        right = self.send(child, 'tools/call', {'name': 'shin_query', 'arguments': {'project': b, 'query': 'readLimit'}})
        qb = json.loads(self.response(child, right)['result']['content'][0]['text'])
        qa = json.loads(self.response(child, left)['result']['content'][0]['text'])
        for query, root, handle, body in ((qa, self.root, a, 'return 7;'), (qb, other, b, 'return 99;')):
            self.assertTrue(query['ok'], query)
            self.assertEqual(query['root'], str(root))
            self.assertEqual(query['mcp_project'], handle)
            self.assertIn(body, query['source_context'])
            args = {'project': handle, 'path': 'limits.js', 'snapshot': query['mcp_snapshot']}
            self.assertTrue(self.tool(child, 'shin_inspect', args)['ok'])
            self.assertIn(body, str(self.tool(child, 'shin_read', args)))
        self.assertEqual(self.tool(child, 'shin_query', {'project': a, 'query': 'readLimit'})['root'], str(self.root))
        for snapshot in (qa['mcp_snapshot'], qb['snapshot_id']):
            refused = self.tool(child, 'shin_read', {'project': b, 'path': 'limits.js', 'snapshot': snapshot})
            self.assertEqual(refused['error']['code'], 'mcp-snapshot-scope-mismatch')
            self.assertEqual(refused['recovery']['kind'], 'request-correction')
            self.assertNotIn('retry_after_ms', refused['recovery'])
            self.assertTrue(refused['recovery']['same_project_required'])
        outside = self.tool(child, 'shin_read', {'project': a, 'path': str(hidden / 'limits.js'),
                                                 'snapshot': qa['mcp_snapshot']})
        self.assertFalse(outside['ok'])
        self.assertNotIn('return 123', json.dumps(outside))
        # Reconnect without importing a mutable current project; the explicit
        # scope and unchanged snapshot receipt remain usable.
        reopened = self.adapter(roots=[other, self.root])
        deadline = time.monotonic() + 10
        while True:
            continued = self.tool(reopened, 'shin_read', {'project': a, 'path': 'limits.js', 'snapshot': qa['mcp_snapshot']})
            if continued['ok']:
                break
            self.assertEqual(continued['error']['code'], 'project-updating', continued)
            self.assertEqual(continued['recovery']['kind'], 'transient')
            self.assertTrue(continued['recovery']['fresh_snapshot_required'])
            self.assertLess(time.monotonic(), deadline, continued)
            threading.Event().wait(.02)
        (other / 'limits.js').write_text('export function readLimit() { return 101; }\n')
        deadline = time.monotonic() + 20
        while True:
            changed = self.tool(child, 'shin_query', {'project': b, 'query': 'readLimit'})
            if changed.get('ok') and 'return 101;' in changed.get('source_context', ''):
                break
            self.assertLess(time.monotonic(), deadline, changed)
            threading.Event().wait(.05)
        self.assertNotEqual(changed['mcp_snapshot'], qb['mcp_snapshot'])
        stale = self.tool(child, 'shin_read', {'project': b, 'path': 'limits.js', 'snapshot': qb['mcp_snapshot']})
        self.assertFalse(stale['ok'])
        self.assertNotIn('source_context', stale)
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        lifecycle = workspace / 'lifecycle'
        lifecycle.mkdir(mode=0o700)
        identity = json.loads((workspace / 'state.json').read_text())['workspace_identity']
        state = {'schema_version': 'shin.local-lifecycle.v1', 'root': str(self.root),
                 'workspace_identity': identity, 'phase': 'detached', 'revision': 1}
        # Lifecycle fixture: no consumer metadata is modified. The native scope
        # test separately exercises the normal lifecycle writer.
        path = lifecycle / 'state.json'
        path.write_text(json.dumps(state)); path.chmod(0o600)
        denied = self.tool(child, 'shin_status', {'project': a})
        self.assertEqual(denied['error']['code'], 'project-detached')
        self.assertEqual(self.tool(child, 'shin_query', {'project': a, 'query': 'readLimit'})['error']['code'], 'project-detached')
        deadline = time.monotonic() + 10
        while True:
            neighbor = self.tool(child, 'shin_status', {'project': b})
            if neighbor['ok']:
                break
            self.assertEqual(neighbor['error']['code'], 'project-updating', neighbor)
            self.assertLess(time.monotonic(), deadline, neighbor)
            threading.Event().wait(.02)
        listed = self.tool(child, 'shin_projects', {})
        self.assertFalse(next(r for r in listed['projects'] if r['project'] == a)['binding_active'])

    def test_multi_project_mismatched_service_provenance_is_discarded(self):
        other = self.base / 'other'
        other.mkdir()
        (other / 'README.md').write_text('# Other\n')
        self.init(root=other)
        listener = self.delayed_service()
        child = self.adapter(roots=[self.root, other])
        handle = self.tool(child, 'shin_projects', {})['projects'][0]['project']
        request_id = self.send(child, 'tools/call', {'name': 'shin_query',
                               'arguments': {'project': handle, 'query': 'readLimit'}})
        connection, _ = listener.accept()
        self.addCleanup(connection.close)
        connection.settimeout(2)
        request = self.receive(connection)
        reply = {k: request[k] for k in ('schema_version', 'request_id', 'observer_version', 'core_version')}
        reply.update({'ok': True, 'result': {'ok': True, 'root': str(other), 'source_context': 'foreign-body',
                                             'snapshot_id': 'fnv1a64:0123456789abcdef'}})
        connection.sendall((json.dumps(reply) + '\n').encode())
        response = self.response(child, request_id)['result']
        self.assertTrue(response['isError'])
        self.assertEqual(json.loads(response['content'][0]['text'])['error']['code'], 'mcp-response-scope-mismatch')
        self.assertNotIn('foreign-body', json.dumps(response))

    def test_multi_project_revocation_during_reply_discards_body(self):
        other = self.base / 'other'
        other.mkdir()
        (other / 'README.md').write_text('# Other\n')
        self.init(root=other)
        listener = self.delayed_service()
        child = self.adapter(roots=[self.root, other])
        handle = self.tool(child, 'shin_projects', {})['projects'][0]['project']
        request_id = self.send(child, 'tools/call', {'name': 'shin_query',
                               'arguments': {'project': handle, 'query': 'readLimit'}})
        connection, _ = listener.accept()
        self.addCleanup(connection.close)
        connection.settimeout(2)
        request = self.receive(connection)
        self.assertEqual(request['root'], str(self.root))
        workspace = self.home / 'projects' / hashlib.sha256(str(self.root).encode()).hexdigest()
        lifecycle = workspace / 'lifecycle'
        lifecycle.mkdir(mode=0o700)
        identity = json.loads((workspace / 'state.json').read_text())['workspace_identity']
        path = lifecycle / 'state.json'
        path.write_text(json.dumps({'schema_version': 'shin.local-lifecycle.v1', 'root': str(self.root),
                                   'workspace_identity': identity, 'phase': 'detached', 'revision': 1}))
        path.chmod(0o600)
        reply = {k: request[k] for k in ('schema_version', 'request_id', 'observer_version', 'core_version')}
        reply.update({'ok': True, 'result': {'ok': True, 'source_context': 'must-not-escape',
                                             'snapshot_id': 'fnv1a64:0123456789abcdef'}})
        connection.sendall((json.dumps(reply) + '\n').encode())
        response = self.response(child, request_id)['result']
        self.assertTrue(response['isError'])
        value = json.loads(response['content'][0]['text'])
        self.assertEqual(value['error']['code'], 'project-detached')
        self.assertNotIn('must-not-escape', json.dumps(response))

    def native_companion(self, binary, *args):
        # Installed kits exclude fixture publishers; production companions stay installed.
        executable = os.environ.get('SHIN_TEST_CORE', str(service.fixtures.TOOL.parent / binary)) \
            if binary == 'shin-observer-test-core' else str(service.fixtures.TOOL.parent / binary)
        run = subprocess.run([executable, *args], capture_output=True,
                             text=True, timeout=45, env={**os.environ, 'PATH': '', 'LC_ALL': 'C'})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def publish_route(self, fixture):
        self.native_companion('shin-observer-test-core', '--profile', 'observer-fixture',
                              '--profile-dir', str(fixture.profiles), '--root', str(fixture.root),
                              '--out-dir', str(fixture.out), 'build')

    def permanent_route(self, managed=False):
        fixture = Fixture(self.base, 'external')
        fixture.write_source('# Limits\n\nreadLimit owns the retry ceiling.\n')
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 23; }\n')
        profile = json.loads(fixture.profile_path.read_text())
        profile.update(bootstrapOnly=False, sourceFactRoots=['.'], sourceFactExtensions=['.js'],
                       evalCases=[{'id': 'limit', 'query': 'retry ceiling', 'mustPaths': ['README.md']}],
                       packEvalCases=[{'id': 'limit-pack', 'mustPaths': ['README.md'], 'expectedComplete': True,
                           'request': {'schema_version': 'shin.task-request.v1', 'request_id': 'limit-pack',
                               'task': {'objective': 'Find retry ceiling', 'must': [{'id': 'limit', 'query': 'retry ceiling'}]},
                               'budget': {'max_evidence_bytes': 8192, 'max_records': 4,
                                          'per_query_records': 4, 'per_document_records': 2}}}])
        fixture.profile_path.write_text(json.dumps(profile))
        self.publish_route(fixture)
        self.native_companion('shin-observer', 'sample', '--config', str(fixture.config))
        self.run_ce('init', '--observer-config', str(fixture.config),
                    *(['--manage-index'] if managed else []), root=fixture.root)
        self.roots.append(fixture.root)
        return fixture

    def test_external_route_mcp_reads_follow_owner_publication_without_reinit(self):
        fixture = self.permanent_route()
        def canonical_files():
            return {str(p): (p.read_bytes(), p.stat().st_mtime_ns) for p in fixture.base.rglob('*') if p.is_file()}
        self.start()
        before = canonical_files()
        child = self.adapter(fixture.root)
        first = self.tool(child, 'shin_query', {'query': 'readLimit'})
        self.assertTrue(first['canonical_ready'])
        self.assertIn('return 23', first['source_context'])
        self.assertEqual(before, canonical_files())
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 31; }\n')
        stale = self.tool(child, 'shin_query', {'query': 'readLimit'})
        self.assertFalse(stale['ok'])
        self.assertNotIn('source_context', stale)
        self.publish_route(fixture)
        published = canonical_files()
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            current = self.tool(child, 'shin_query', {'query': 'readLimit'})
            if current['ok']:
                break
            time.sleep(0.1)
        self.assertTrue(current['ok'])
        self.assertIn('return 31', current['source_context'])
        self.assertNotEqual(first['snapshot_id'], current['snapshot_id'])
        read = self.tool(child, 'shin_read', {'path': 'limits.js', 'snapshot': current['snapshot_id']})
        self.assertIn('return 31', read['source_context'])
        self.assertEqual(published, canonical_files())

    def test_membership_candidate_partial_context_and_automatic_recovery(self):
        fixture = self.permanent_route(managed=True)
        self.start()
        client = self.adapter(fixture.root)
        original = json.loads(fixture.profile_path.read_text())
        changed = {**original, 'rootFiles': []}
        fixture.profile_path.write_text(json.dumps(changed))
        retry = threading.Event()
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            partial = self.tool(client, 'shin_status', {})
            if partial.get('ok') and partial.get('source_coverage', {}).get('reason') == 'profile-revision-pending':
                break
            retry.wait(.05)
        else:
            self.fail('partial managed reader unavailable: ' + str(partial))
        self.assertFalse(partial['canonical_ready'])
        self.assertTrue(partial['retrieval_ready'])
        read = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': partial['snapshot_id']})
        self.assertTrue(read['ok'])
        self.assertIn('return 23;', read['source_context'])
        (fixture.root / 'limits.js').unlink()
        omitted = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': partial['snapshot_id']})
        self.assertTrue(omitted['ok'])
        self.assertFalse(omitted['native_read_complete'])
        self.assertNotIn('return 23;', omitted['source_context'])
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 88; }\n')
        (fixture.root / 'extra.md').write_text('# Extra source\n\nFresh description of readLimit.\n')
        fixture.profile_path.write_text(json.dumps({**original, 'rootFiles': [*original['rootFiles'], 'extra.md']}))
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            ready = self.tool(client, 'shin_query', {'query': 'readLimit'})
            if ready.get('canonical_ready') and 'return 88;' in ready.get('source_context', ''):
                break
            retry.wait(.05)
        else:
            self.fail('automatic membership revision did not publish: ' + str(ready))
        self.assertIsNone(client.poll())
        self.assertNotEqual(ready['snapshot_id'], partial['snapshot_id'])

    def test_active_edits_retry_and_recover_in_same_mcp_session(self):
        fixture = self.permanent_route(managed=True)
        self.start()
        client = self.adapter(fixture.root)
        stopped = threading.Event()
        def edit():
            value = 100
            while not stopped.is_set():
                pending = fixture.root / 'limits.pending'
                pending.write_text('export function readLimit() { return ' + str(value) + '; }\n')
                pending.replace(fixture.root / 'limits.js')
                value += 1
                stopped.wait(.025)
        writer = threading.Thread(target=edit)
        writer.start()
        try:
            deadline = time.monotonic() + 4
            retries = 0
            while time.monotonic() < deadline:
                result = self.tool(client, 'shin_query', {'query': 'readLimit'}, timeout=35)
                if not result['ok']:
                    self.assertIn(result['error']['code'], ('project-updating', 'service-busy', 'query-source-changed',
                                                           'source-read-hash-mismatch', 'file-changed-during-verification'))
                    self.assertNotIn('source_context', result)
                    retries += 1
                stopped.wait(.05)
            self.assertGreater(retries, 0)
        finally:
            stopped.set()
            writer.join(timeout=5)
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 9876; }\n')
        deadline = time.monotonic() + 20
        retry = threading.Event()
        while time.monotonic() < deadline:
            result = self.tool(client, 'shin_query', {'query': 'readLimit'}, timeout=35)
            if result['ok'] and 'return 9876;' in result['source_context']:
                break
            retry.wait(.05)
        else:
            self.fail('active-edit route did not recover: ' + str(result))
        self.assertIsNone(client.poll())
        read = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': result['snapshot_id']})
        self.assertTrue(read['ok'])
        self.assertIn('return 9876;', read['source_context'])

    def test_source_permission_diagnostic_and_recovery(self):
        fixture = self.permanent_route(managed=True)
        self.start()
        client = self.adapter(fixture.root)
        source = fixture.root / 'limits.js'
        mode = source.stat().st_mode & 0o777
        source.chmod(0)
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                result = self.tool(client, 'shin_query', {'query': 'readLimit'}, timeout=35)
                if result.get('error', {}).get('code') == 'access-denied' and 'diagnostic' in result['error']:
                    break
                threading.Event().wait(.05)
            else:
                self.fail('permission diagnostic did not reach MCP: ' + str(result))
            self.assertNotIn('source_context', result)
            self.assertEqual(result['recovery']['kind'], 'environment-action')
            self.assertNotIn('retry_after_ms', result['recovery'])
            detail = result['error']['diagnostic']
            self.assertEqual(detail['category'], 'access-denied')
            self.assertNotIn(str(fixture.root), json.dumps(detail))
        finally:
            source.chmod(mode)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            result = self.tool(client, 'shin_query', {'query': 'readLimit'}, timeout=35)
            if result['ok']:
                break
            threading.Event().wait(.05)
        else:
            self.fail('permission restoration did not recover: ' + str(result))
        self.assertIsNone(client.poll())

    def test_profile_change_guidance_requires_review_then_same_mcp_recovers(self):
        fixture = self.permanent_route(managed=True)
        self.start()
        client = self.adapter(fixture.root)
        self.assertTrue(self.tool(client, 'shin_query', {'query': 'readLimit'})['ok'])
        manifest = fixture.out / 'docs/kb/generated/manifest.json'
        original = manifest.read_bytes()
        profile = json.loads(fixture.profile_path.read_text())
        (fixture.root / 'firestore.md').write_text('# Firestore\n\nNative firestore execution.\n')
        profile['rootFiles'].append('firestore.md')
        profile['documentIncludePrefixes'] = ['README.md', 'firestore.md']
        fixture.profile_path.write_text(json.dumps(profile))
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            blocked = self.tool(client, 'shin_status', {})
            if blocked.get('error', {}).get('code') == 'route-revision-conflict':
                break
            time.sleep(.05)
        else:
            self.fail('profile change was not fenced: ' + str(blocked))
        self.assertEqual(manifest.read_bytes(), original)
        self.assertNotIn('source_context', blocked)
        hint = blocked['recovery']
        self.assertEqual(hint['kind'], 'operator-review')
        self.assertTrue(hint['approval_required'])
        self.assertTrue(hint['same_project_required'])
        self.assertTrue(hint['fresh_snapshot_required'])
        self.assertNotIn('retry_after_ms', hint)
        self.assertNotIn('next_tool', hint)
        self.assertIn('profile-review', hint['message'])
        # Status/query do not approve the profile; the exact owner action is separate.
        review = self.run_ce('profile-review', root=fixture.root)['profile_review']
        self.assertTrue(review['changed'])
        self.run_ce('profile-accept', review['revision'], '--review-token', review['review_token'], root=fixture.root)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            result = self.tool(client, 'shin_query', {'query': 'readLimit'}, timeout=35)
            if result['ok']:
                break
            time.sleep(.05)
        else:
            self.fail('same MCP did not recover after approval: ' + str(result))
        self.assertTrue(result['canonical_ready'])
        self.assertIsNone(client.poll())
        read = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': result['snapshot_id']})
        self.assertTrue(read['native_read_complete'])

    def test_managed_refresh_preserves_gate_reasons_and_recovers(self):
        fixture = self.permanent_route(managed=True)
        self.start()
        client = self.adapter(fixture.root)
        original = (fixture.root / 'README.md').read_text()
        manifest = fixture.out / 'docs/kb/generated/manifest.json'
        accepted = manifest.read_bytes()
        (fixture.root / 'README.md').write_text(original + '\n[Unpublished document](missing-private-document.md)\n')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            blocked = self.tool(client, 'shin_status', {})
            if blocked.get('error', {}).get('code') == 'refresh-validation-failed':
                break
            time.sleep(.05)
        else:
            self.fail('broken-link refresh refusal was not observed: ' + str(blocked))
        self.assertEqual(manifest.read_bytes(), accepted)
        self.assertNotIn('source_context', blocked)
        diagnostic = blocked['error']['diagnostic']
        doctor = next(g for g in diagnostic['gates'] if g['id'] == 'doctor')
        self.assertIn('broken-local-link', doctor['reasons'])
        self.assertIn('broken-local-link', blocked['error']['message'])
        self.assertIn('Report this blocker', blocked['error']['message'])
        self.assertNotIn('missing-private-document', json.dumps(diagnostic))
        native = self.run_ce('status', '--service', root=fixture.root, expected=2)
        self.assertEqual(native['error']['diagnostic'], diagnostic)
        project = next(p for p in self.raw('service-status')['result']['telemetry']['project_states']
                       if p['root'] == str(fixture.root))
        self.assertTrue(project['active_issue'])
        self.assertEqual(project['validation_diagnostic'], diagnostic)
        (fixture.root / 'README.md').write_text(original)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            current = self.tool(client, 'shin_query', {'query': 'readLimit'})
            if current['ok']:
                self.assertIn('return 23;', current['source_context'])
                break
            time.sleep(.05)
        else:
            self.fail('managed query did not recover after link repair: ' + str(current))
        project = next(p for p in self.raw('service-status')['result']['telemetry']['project_states']
                       if p['root'] == str(fixture.root))
        self.assertFalse(project['active_issue'])

    def test_managed_session_survives_restart_stale_snapshot_and_quality_failure(self):
        fixture = self.permanent_route(managed=True)
        external_state = (fixture.state / 'state.json').read_bytes()
        owner = (fixture.out / 'docs/kb/.shin-context-owner').read_bytes()
        daemon = self.start()
        client = self.adapter(fixture.root)
        other = self.adapter()
        initial = self.tool(client, 'shin_query', {'query': 'readLimit'})
        self.assertTrue(initial['ok'])
        self.assertIn('return 23;', initial['source_context'])
        self.assertEqual(initial['index'], str(fixture.out))
        initial_service = self.raw('service-status')['result']['instance_id']

        def current(value):
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                result = self.tool(client, 'shin_query', {'query': 'readLimit'})
                if result['ok']:
                    self.assertIn(f'return {value};', result['source_context'])
                    return result
                self.assertNotIn('source_context', result)
                time.sleep(.05)
            self.fail('managed MCP query did not recover: ' + str(result))

        self.assertTrue(self.raw('service-stop')['ok'])
        daemon.communicate(timeout=15)
        self.assertEqual(daemon.returncode, 0)
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 31; }\n')
        unavailable = self.tool(client, 'shin_query', {'query': 'readLimit'})
        self.assertFalse(unavailable['ok'])
        self.assertEqual(unavailable['error']['code'], 'service-unavailable')
        self.assertNotIn('source_context', unavailable)
        self.assertIsNone(client.poll())
        self.assertEqual(self.rpc(client, 'ping', {})['result'], {})
        self.start()
        self.assertNotEqual(self.raw('service-status')['result']['instance_id'], initial_service)
        refreshed = current(31)
        self.assertNotEqual(refreshed['snapshot_id'], initial['snapshot_id'])
        stale_read = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': initial['snapshot_id']})
        self.assertFalse(stale_read['ok'])
        self.assertNotIn('source_context', stale_read)
        read = self.tool(client, 'shin_read', {'path': 'limits.js', 'snapshot': refreshed['snapshot_id']})
        self.assertTrue(read['ok'])
        self.assertIn('return 31;', read['source_context'])

        manifest = fixture.out / 'docs/kb/generated/manifest.json'
        accepted = manifest.read_bytes()
        original = (fixture.root / 'README.md').read_text()
        (fixture.root / 'README.md').write_text('# Unrelated\n\nNo required contract evidence.\n')
        watch = self.home / 'projects' / hashlib.sha256(str(fixture.root).encode()).hexdigest() / 'watch/state.json'
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            state = json.loads(watch.read_text())
            if state.get('last_result', {}).get('error', {}).get('code') == 'refresh-validation-failed':
                break
            time.sleep(.05)
        else:
            self.fail('permanent profile quality refusal was not observed: ' + str(state))
        self.assertEqual(manifest.read_bytes(), accepted)
        blocked = self.tool(client, 'shin_query', {'query': 'readLimit'})
        self.assertFalse(blocked['ok'])
        self.assertNotIn('source_context', blocked)
        self.assertEqual(blocked['error']['code'], 'refresh-validation-failed')
        diagnostic = blocked['error']['diagnostic']
        self.assertEqual(diagnostic['schema_version'], 'shin.validation-diagnostic.v1')
        eval_gate = next(g for g in diagnostic['gates'] if g['id'] == 'eval')
        self.assertTrue(eval_gate['cases'])
        missing_path_case = next(c for c in eval_gate['cases'] if 'missing-required-path' in c['reasons'])
        self.assertIn('README.md', missing_path_case['expected_paths'])
        self.assertTrue((fixture.root / 'README.md').is_file())
        self.assertIn('not found in search results', blocked['error']['message'])
        self.assertIn('does not show that the document is missing from disk', blocked['error']['message'])
        self.assertIn('Expected document: README.md', blocked['error']['message'])
        self.assertIn('validation settings do not need changes', blocked['error']['message'])
        self.assertFalse(any('broken-local-link' in g.get('reasons', []) for g in diagnostic['gates']))
        self.assertEqual(set(diagnostic), {'schema_version', 'gates', 'truncated'})
        for gate in diagnostic['gates']:
            self.assertLessEqual(set(gate), {'id', 'cases', 'error_code', 'reasons'})
            identifiers = [gate['id'], *gate.get('reasons', [])]
            if 'error_code' in gate:
                identifiers.append(gate['error_code'])
            for case in gate['cases']:
                self.assertLessEqual(set(case), {'id', 'reasons', 'expected_paths'})
                self.assertTrue({'id', 'reasons'} <= set(case))
                if 'expected_paths' in case:
                    self.assertLessEqual(len(case['expected_paths']), 2)
                    for path in case['expected_paths']:
                        self.assertFalse(path.startswith('/'))
                        self.assertNotIn('..', path.split('/'))
                identifiers.extend([case['id'], *case['reasons']])
            for identifier in identifiers:
                self.assertRegex(identifier, r'\A[A-Za-z0-9_.-]{1,96}\Z')
        self.assertNotIn('readLimit', json.dumps(diagnostic))
        native = self.run_ce('query', 'readLimit', '--service', root=fixture.root, expected=2)
        self.assertEqual(native['error']['code'], 'refresh-validation-failed')
        self.assertEqual(native['error']['diagnostic'], diagnostic)
        project = next(p for p in self.raw('service-status')['result']['telemetry']['project_states']
                       if p['root'] == str(fixture.root))
        self.assertTrue(project['active_issue'])
        self.assertEqual(project['query_phase'], 'blocked')
        self.assertEqual(project['validation_diagnostic'], diagnostic)
        for name, args in [('shin_status', {}), ('shin_read', {'path': 'limits.js', 'snapshot': refreshed['snapshot_id']}),
                           ('shin_inspect', {'path': 'limits.js', 'snapshot': refreshed['snapshot_id']})]:
            refusal = self.tool(client, name, args)
            self.assertEqual(refusal['error']['code'], 'refresh-validation-failed')
            self.assertEqual(refusal['error']['diagnostic'], diagnostic)
            self.assertNotIn('source_context', refusal)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            state = json.loads(watch.read_text())
            if state['phase'] == 'coalescing':
                break
            time.sleep(.05)
        else:
            self.fail('retry observation was not seen')
        self.assertEqual(self.tool(client, 'shin_query', {'query': 'readLimit'})['error']['code'],
                         'refresh-validation-failed')
        self.assertIn('return 7;', self.tool(other, 'shin_query', {'query': 'readLimit'})['source_context'])

        (fixture.root / 'README.md').write_text(original)
        (fixture.root / 'limits.js').write_text('export function readLimit() { return 47; }\n')
        recovered = current(47)
        project = next(p for p in self.raw('service-status')['result']['telemetry']['project_states']
                       if p['root'] == str(fixture.root))
        self.assertFalse(project['active_issue'])
        self.assertNotEqual(project['query_phase'], 'blocked')
        self.assertNotEqual(recovered['snapshot_id'], refreshed['snapshot_id'])
        self.assertIsNone(client.poll())
        reconnected = self.adapter(fixture.root)
        self.assertEqual(self.tool(reconnected, 'shin_query', {'query': 'readLimit'})['snapshot_id'],
                         recovered['snapshot_id'])
        self.assertEqual(external_state, (fixture.state / 'state.json').read_bytes())
        self.assertEqual(owner, (fixture.out / 'docs/kb/.shin-context-owner').read_bytes())

    def test_missing_service_and_invalid_arguments_fail_closed(self):
        child = self.adapter()
        result = self.tool(child, 'shin_query', {'query': 'readLimit'})
        self.assertFalse(result['ok'])
        self.assertNotIn('source_context', result)
        self.assertFalse(self.tool(child, 'shin_query', {'query': ''})['ok'])
        self.assertFalse(self.tool(child, 'shin_read', {'path': 'limits.js'})['ok'])
        self.assertTrue(self.run_ce('query', 'readLimit')['ok'])

    def test_oversized_and_duplicate_key_frames_close_channel(self):
        for frame in [b'x' * 65537, b'{"jsonrpc":"2.0","id":1,"id":2}\n']:
            child = self.adapter()
            child.stdin.write(frame)
            child.stdin.flush()
            child.wait(timeout=10)
            self.assertEqual(child.returncode, 0)

    def test_cancel_is_prompt_typed_and_does_not_cancel_another_request(self):
        listener = self.delayed_service()
        child = self.adapter()
        first, left = self.pending_query(child, listener)
        second, right = self.pending_query(child, listener)
        self.notify_cancel(child, str(first))
        self.assertIn('result', self.rpc(child, 'ping', {}))
        self.assertEqual(select.select([left, right], [], [], 0.1)[0], [])
        started = time.monotonic()
        self.notify_cancel(child, first)
        self.assertEqual(left.recv(1), b'')
        result = self.response(child, first, timeout=2)['result']
        self.assertTrue(result['isError'])
        self.assertEqual(json.loads(result['content'][0]['text'])['error']['code'], 'service-request-cancelled')
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(select.select([right], [], [], 0.1)[0], [])
        self.notify_cancel(child, second)
        self.assertEqual(right.recv(1), b'')
        self.assertTrue(self.response(child, second, timeout=2)['result']['isError'])

    def test_capacity_recovers_and_conflicting_ids_are_rejected(self):
        listener = self.delayed_service()
        child = self.adapter()
        first, left = self.pending_query(child, listener)
        second, right = self.pending_query(child, listener)
        denied = self.rpc(child, 'tools/call', {'name': 'shin_query', 'arguments': {'query': 'readLimit'}})
        self.assertIn('capacity', denied['error']['message'])
        child.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': str(first), 'method': 'ping'}) + '\n').encode())
        child.stdin.flush()
        self.assertIn('conflicts', self.response(child, str(first), timeout=2)['error']['message'])
        self.notify_cancel(child, first)
        self.assertEqual(left.recv(1), b'')
        self.response(child, first, timeout=2)
        third, replacement = self.pending_query(child, listener)
        for request_id, connection in [(second, right), (third, replacement)]:
            self.notify_cancel(child, request_id)
            self.assertEqual(connection.recv(1), b'')
            self.response(child, request_id, timeout=2)

    def test_eof_and_termination_close_pending_service_calls(self):
        listener = self.delayed_service()
        for action in ['eof', 'terminate']:
            child = self.adapter()
            _, connection = self.pending_query(child, listener)
            started = time.monotonic()
            if action == 'eof':
                child.stdin.close()
                child.stdin = None
            else:
                child.terminate()
            self.assertEqual(connection.recv(1), b'')
            child.wait(timeout=2)
            self.assertEqual(child.returncode, 0)
            self.assertLess(time.monotonic() - started, 2)


if __name__ == '__main__':
    unittest.main()
