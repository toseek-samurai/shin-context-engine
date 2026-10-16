#!/usr/bin/env python3
"""Read-only installed-kit proof; no inference, enrollment or source-body report."""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import threading
import time


def locator(value):
    lines = value['source_context'].splitlines()
    source = next((line for line in lines if line.startswith('SOURCE ')), None)
    if source:
        columns = json.loads(next(line[5:] for line in lines if line.startswith('SHIN ')))['locator_columns']
        record = json.loads(source.split(' ', 2)[2])
        return record[columns.index('path')], 'source'
    for record in value.get('document_context', {}).get('evidence', []):
        if record.get('path'):
            return record['path'], 'document'
    raise RuntimeError('query-returned-no-source-or-document-locator')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ce', type=Path, required=True)
    parser.add_argument('--mcp', type=Path, required=True)
    parser.add_argument('--home', type=Path, required=True)
    parser.add_argument('--project', nargs=2, action='append', metavar=('ROOT', 'QUERY'), required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=1)
    args = parser.parse_args()
    ce = str(args.ce.resolve())
    home = str(args.home.resolve())
    if not 1 <= args.repeats <= 100:
        parser.error('--repeats must be in 1..100')
    done = threading.Event()
    samples, sample_errors, cases = [], [], []

    def sample():
        while not done.is_set():
            try:
                rows = subprocess.run(['/bin/ps', '-axo', 'pid=,ppid=,rss=,command='],
                    capture_output=True, text=True, timeout=3, check=True).stdout.splitlines()
                rows = [row.split(None, 3) for row in rows if len(row.split(None, 3)) == 4]
                supervisors = {int(row[0]) for row in rows if row[3] == f'{ce} _serve-supervised --home {home}'}
                owned = [row for row in rows if int(row[0]) in supervisors or
                    (int(row[1]) in supervisors and row[3] in
                     (f'{ce} _refresh-worker', f'{ce} _service-worker', f'{ce} _resident-worker'))]
                if supervisors:
                    samples.append({'rss_bytes': sum(int(row[2]) * 1024 for row in owned),
                                    'processes': len(owned)})
            except (OSError, subprocess.SubprocessError, ValueError):
                sample_errors.append('sampling-unavailable')
            done.wait(.2)

    def call(root, command):
        start = time.monotonic()
        run = subprocess.run([ce, *command, '--root', root, '--home', home, '--service'],
            capture_output=True, text=True, timeout=45)
        value = json.loads(run.stdout or run.stderr)
        if run.returncode or not value.get('ok'):
            raise RuntimeError(f"{root}: {command[0]}: " + value.get('error', {}).get('code', 'native-result-not-ok'))
        return value, round((time.monotonic() - start) * 1000, 2)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    adapter = None
    try:
        for root, query in args.project * args.repeats:
            value, query_ms = call(root, ['query', query])
            assert value['root'] == root
            snapshot = value['snapshot_id']
            path, evidence_kind = locator(value)
            inspected, read, inspect_ms, read_ms = {}, {}, None, None
            if evidence_kind == 'source':
                inspected, inspect_ms = call(root, ['inspect', path, '--snapshot', snapshot])
                read, read_ms = call(root, ['read', path, '--snapshot', snapshot])
                assert inspected['snapshot_id'] == snapshot == read['snapshot_id']
            else:
                # Native source-read accepts source-fact locators, not every document.
                document = (Path(root) / path).resolve(strict=True)
                assert document.is_relative_to(Path(root).resolve())
                with document.open('rb') as stream:
                    assert stream.read(65536), 'empty document evidence'
            cases.append({'root': root, 'version': value['version'], 'core_version': value['core_version'],
                'followup': 'native-inspect-read' if evidence_kind == 'source' else 'direct-document-read',
                'snapshot_id': snapshot, 'path': path, 'query_ms': query_ms, 'inspect_ms': inspect_ms,
                'read_ms': read_ms, 'native_read_complete': read.get('native_read_complete'), 'ok': True,
                'document_records': len(value['document_context']['evidence']),
                'transport_ms': {key: result.get('transport_duration_ms') for key, result in
                    [('query', value), ('inspect', inspected), ('read', read)]},
                'freshness_contract': value.get('freshness_contract'),
                'work_performed': {key: result.get('work_performed') for key, result in
                    [('query', value), ('inspect', inspected), ('read', read)]},
                'phase_ms': {key: result.get('phase_ms') for key, result in
                    [('query', value), ('inspect', inspected), ('read', read)]}})
        root, query = args.project[0]
        adapter = subprocess.Popen([str(args.mcp.resolve()), '--root', root, '--home', home],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sequence, pending = 0, b''

        def rpc(method, params):
            nonlocal sequence, pending
            sequence += 1
            adapter.stdin.write((json.dumps({'jsonrpc': '2.0', 'id': sequence, 'method': method, 'params': params}) + '\n').encode())
            adapter.stdin.flush()
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    value = json.loads(line)
                    if value.get('id') == sequence:
                        assert 'error' not in value
                        return value['result']
                if select.select([adapter.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                    chunk = os.read(adapter.stdout.fileno(), 8192)
                    assert chunk, 'native MCP EOF'
                    pending += chunk
                    assert len(pending) <= 1024 * 1024
            raise RuntimeError('native-mcp-timeout')

        initialized = rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'shin-installed-qualification', 'version': '1'}})
        assert initialized['serverInfo']['name'] == 'shin'
        adapter.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        adapter.stdin.flush()
        catalog = rpc('tools/list', {})['tools']
        assert {tool['name'] for tool in catalog} == {'shin_status', 'shin_query', 'shin_inspect', 'shin_read', 'shin_help'}
        assert all(tool['annotations']['readOnlyHint'] for tool in catalog)
        for name, arguments in [('shin_status', {}), ('shin_query', {'query': query})]:
            result = rpc('tools/call', {'name': name, 'arguments': arguments})
            assert not result.get('isError')
            assert json.loads(result['content'][0]['text'])['ok']
        adapter.stdin.close()
        adapter.stdin = None
        adapter.wait(timeout=10)
        assert adapter.returncode == 0
    finally:
        if adapter and adapter.poll() is None:
            adapter.kill()
            adapter.communicate(timeout=5)
        done.set()
        sampler.join(timeout=4)
    report = {'schema_version': 'shin.installed-local-probe.v1', 'ok': True, 'cases': cases,
        'native_mcp': 'catalog-status-query-eof-passed', 'model_called': False,
        'resource_scope': 'sampled-sum-supervisor-and-direct-native-workers-not-capacity-gate',
        'sample_interval_ms': 200, 'samples': len(samples), 'sampling_errors': len(sample_errors),
        'sampled_peak_rss_bytes': max((row['rss_bytes'] for row in samples), default=None),
        'max_sampled_processes': max((row['processes'] for row in samples), default=None)}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open('x') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
