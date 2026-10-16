"""Private bound search executor. Reject foreign revision/content before delivery."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def verify_source(root, hashes):
    actual = {}
    for path in root.rglob('*'):
        if path.is_symlink():
            raise ValueError('source-link')
        if path.is_file():
            actual[str(path.relative_to(root))] = sha(path.read_bytes())
    if actual != hashes:
        raise ValueError('source-changed')


def verified_records(root, hashes, rows):
    records = []
    for row in rows:
        name, text = row['path'], row['text']
        if name not in hashes or not name.startswith('src/') or not isinstance(text, str) or not text:
            raise ValueError('foreign-or-empty-evidence')
        source = (root / name).read_text()
        offset = source.find(text)
        if offset < 0:
            raise ValueError('evidence-content-mismatch')
        line = source[:offset].count('\n') + 1
        if row.get('line') is not None and row['line'] != line:
            # Repeated identical snippets are valid only at the declared line.
            offset = sum(len(s) for s in source.splitlines(keepends=True)[:row['line'] - 1])
            if not source[offset:].startswith(text):
                raise ValueError('evidence-line-mismatch')
            line = row['line']
        records.append({'path': name, 'line': line, 'end_line': line + text.count('\n'), 'text': text})
    return records


def bounded_result(provider, records, maximum):
    result = {'provider': provider, 'source_verified': True, 'answer_complete': False,
              'results': [], 'omitted': len(records)}
    for record in records:
        candidate = {**result, 'results': result['results'] + [record], 'omitted': result['omitted'] - 1}
        if len(json.dumps(candidate, ensure_ascii=False).encode()) + 1 > maximum:
            break
        result = candidate
    raw = json.dumps(result, ensure_ascii=False) + '\n'
    if len(raw.encode()) > maximum:
        raise ValueError('output-budget')
    return raw


def search(state_path, provider, query, maximum):
    state = json.loads(state_path.read_text())
    config, root = state['comparison'], Path(state['root'])
    if provider not in config['providers'] or not query.strip() or len(query.encode()) > 4096:
        raise ValueError('provider-or-query')
    verify_source(root, config['source_hashes'])
    start = time.monotonic()
    if provider == 'shin':
        binding = config['shin']
        if sha(Path(binding['config']).read_bytes()) != binding['config_sha256']:
            raise ValueError('config-changed')
        if Path(json.loads(Path(binding['config']).read_text())['root']).resolve() != root.resolve():
            raise ValueError('foreign-binding')
        argv = [binding['observer'], 'source-context', '--config', binding['config'], '--query', query]
        env = {'PATH': '', 'LC_ALL': 'C'}
    else:
        argv = [config['jbcontext'], 'search', '--project-path', str(root), '--git-remote-url', config['repository'],
            '--revision', config['revision'], '-p', 'src/', '--limit', '5', '--json-output', query]
        env = {'PATH': '/opt/homebrew/bin:/usr/bin:/bin', 'HOME': config['credential_home'], 'LC_ALL': 'C'}
    # The native provider is trusted; model code never runs in this process.
    completed = subprocess.run(argv, capture_output=True, env=env, timeout=25)
    if completed.returncode or len(completed.stdout) > 2097152:
        raise ValueError('provider-failed-or-over-budget')
    result = json.loads(completed.stdout)
    if provider == 'shin':
        native = result['result']['source_context']
        if native['snapshot_id'] != binding['snapshot_id']:
            raise ValueError('snapshot-changed')
        rows = [{'path': b['path'], 'text': b['text'], 'line': b['start_line']} for b in native['context']['bodies']]
    else:
        if result.get('revision') != config['revision']:
            raise ValueError('foreign-or-missing-revision')
        rows = [{'path': r['result']['sourcePosition']['relativePath'], 'text': r['content'],
                 'line': r.get('contentStartLine')} for r in result.get('results', [])]
    records = verified_records(root, config['source_hashes'], rows)
    verify_source(root, config['source_hashes'])
    delivered = bounded_result(provider, records, maximum)
    audit = {'provider': provider, 'query': query, 'elapsed_ms': round((time.monotonic() - start) * 1000, 3),
             'response_sha256': sha(completed.stdout), 'delivered_sha256': sha(delivered.encode()),
             'verified_records': len(records), 'revision': config['revision']}
    folder = state_path.parent / 'comparison-audit'
    folder.mkdir(exist_ok=True)
    (folder / (str(time.time_ns()) + '.json')).write_text(json.dumps(audit))
    return delivered


if __name__ == '__main__':
    try:
        print(search(Path(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])), end='')
    except Exception as error:
        # Never forward provider stderr/auth diagnostics or unverified snippets.
        print(json.dumps({'ok': False, 'error': str(error) if isinstance(error, ValueError) else 'comparison-search-failed'}))
        raise SystemExit(1)
