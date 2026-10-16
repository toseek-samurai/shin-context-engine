#!/usr/bin/env python3
"""Operator evidence helper; not shipped runtime or an automatic collector.

Read only existing numerical Shin history and model-usage projections. Persist
bounded incremental slices outside project roots; never read model transcripts.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import time
import uuid

COLUMNS = ('id,run,at_ms,kind,project,phase,method,outcome,duration,bytes,rss,workers,'
           'service_duration,reader_state,purpose,error_code,admission,documents,'
           'source,source_collection,final_verification').split(',')
OPTIONAL_COLUMNS = 'request_id,session_id,parent_request_id,generation_id,follow_up,stage,wait_ms,admission_retries,refresh_phases'.split(',')


def capture(database, usage_state, kit, output, label):
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    if output.is_symlink() or output.stat().st_uid != os.getuid() or output.stat().st_mode & 0o077:
        raise ValueError('Output must be a private user-owned directory')
    with (output / '.capture.lock').open('a') as lock:
        os.chmod(lock.name, 0o600)
        fcntl.flock(lock, fcntl.LOCK_EX)
        return capture_locked(database, usage_state, kit, output, label)


def capture_locked(database, usage_state, kit, output, label):
    captures = sorted(output.glob('capture-*.json'))
    previous = json.loads(captures[-1].read_text()) if captures else None
    identity = {'path': str(database.resolve(strict=True)), 'device': database.stat().st_dev,
                'inode': database.stat().st_ino}
    if previous and previous['database'] != identity:
        raise ValueError('History database identity changed; use a new reviewed analysis directory')
    manifest = json.loads((kit / 'shin-kit.json').read_text())
    usage = json.loads(usage_state.read_text())
    if usage.get('schema_version') != 'shin.model-usage.v3':
        raise ValueError('Unsupported numerical usage schema')
    if previous and previous['model_usage'].get('config_sha256') != usage.get('config_sha256'):
        raise ValueError('Usage mapping changed; use a new reviewed analysis directory')
    with sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True, timeout=5) as db:
        db.execute('BEGIN')
        if (db.execute('PRAGMA application_id').fetchone()[0] != 1397246280 or
                db.execute('PRAGMA user_version').fetchone()[0] != 2):
            raise ValueError('Unsupported numerical history schema')
        low, high = db.execute('SELECT min(id),max(id) FROM events').fetchone()
        last = previous['through_id'] if previous else (high or 0)
        if previous and high is not None and high < last:
            raise ValueError('History sequence moved backward; use a new reviewed analysis directory')
        available = {row[1] for row in db.execute('PRAGMA table_info(events)')}
        columns = COLUMNS + [column for column in OPTIONAL_COLUMNS if column in available]
        rows = db.execute('SELECT ' + ','.join(columns) + ' FROM events WHERE id>? ORDER BY id LIMIT 20001', (last,)).fetchall()
        if len(rows) > 20000:
            raise ValueError('History exceeds the qualified row bound')
    now = time.time_ns() // 1000000
    if previous and now < previous['captured_at_ms']:
        raise ValueError('Clock moved backward; resolve before appending captures')
    sequence = previous['sequence'] + 1 if previous else 1
    value = {'schema_version': 'shin.review-capture.v1', 'sequence': sequence, 'label': label,
             'captured_at_ms': now, 'baseline': previous is None, 'database': identity,
             'after_id': last, 'through_id': high if high is not None else last,
             'retained_min_id': low, 'missing_rows_before_slice': max(0, low-last-1) if low is not None and previous else 0,
             'missing_rows_in_slice': max(0, (high if high is not None else last)-last-len(rows)) if previous else 0,
             'columns': columns, 'events': rows,
             'kit': {'version': manifest['version'], 'core_version': manifest['core_version']},
             'model_usage': usage,
             'limits': {'history_scope': 'all service activity; optional explicit chat correlation, missing identity is unknown',
                        'model_scope': 'session-origin projection, not operation attribution',
                        'model_delta_may_decrease': True}}
    stamp = datetime.datetime.fromtimestamp(now/1000, datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    target = output / ('capture-' + f'{sequence:08d}-' + stamp + '-' + uuid.uuid4().hex[:8] + '.json')
    temporary = output / ('.' + target.name + '.tmp')
    with temporary.open('x') as stream:
        os.chmod(temporary, 0o600)
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)
    return target, value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('home', 'usage-state', 'kit', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--label', required=True)
    args = parser.parse_args()
    path, value = capture(args.home/'service/history/telemetry.sqlite', args.usage_state,
                          args.kit, args.out, args.label)
    print(json.dumps({'file': str(path), 'baseline': value['baseline'],
                      'new_rows': len(value['events']), 'through_id': value['through_id'],
                      'missing_rows_before_slice': value['missing_rows_before_slice']}))


if __name__ == '__main__':
    main()
