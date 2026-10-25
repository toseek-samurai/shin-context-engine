#!/usr/bin/env python3
"""Operator evidence helper; not shipped runtime or an automatic collector.

Read only existing numerical Shin history and model-usage projections. Persist
bounded incremental slices outside project roots; never read model transcripts.
"""
import argparse
import datetime
import ctypes
from contextlib import closing
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import struct
import sys
import time
import uuid

COLUMNS = ('id,run,at_ms,kind,project,phase,method,outcome,duration,bytes,rss,workers,'
           'service_duration,reader_state,purpose,error_code,admission,documents,'
           'source,source_collection,final_verification').split(',')
OPTIONAL_COLUMNS = 'request_id,session_id,parent_request_id,generation_id,follow_up,stage,wait_ms,admission_retries,refresh_phases'.split(',')


def database_identity(database):
    path = database.resolve(strict=True)
    with path.open('rb') as stream:
        status = os.fstat(stream.fileno())
        value = {'path': str(path), 'device': status.st_dev, 'inode': status.st_ino}
        if sys.platform == 'darwin':
            # Same ATTR_VOL_UUID contract as the native filesystem identity.
            class AttrList(ctypes.Structure):
                _fields_ = [('bitmapcount', ctypes.c_ushort), ('reserved', ctypes.c_ushort)] + [
                    (name, ctypes.c_uint32) for name in ('common', 'volume', 'directory', 'file', 'fork')]
            attributes = AttrList(5, 0, 0, 0x80040000, 0, 0, 0)
            buffer = ctypes.create_string_buffer(20)
            native = ctypes.CDLL(None, use_errno=True).fgetattrlist
            native.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
            native.restype = ctypes.c_int
            if native(stream.fileno(), ctypes.byref(attributes), buffer, len(buffer), 0) != 0:
                raise OSError(ctypes.get_errno(), 'Persistent history volume identity unavailable')
            length, volume = struct.unpack('=I16s', buffer.raw)
            if length != 20 or not any(volume):
                raise ValueError('Persistent history volume identity unavailable')
            value['volume_uuid'] = str(uuid.UUID(bytes=volume))
        return value


def review_identity(previous, current, accept_remount):
    old = previous['database']
    keys = ('path', 'inode', 'volume_uuid') if 'volume_uuid' in old else ('path', 'inode', 'device')
    if all(old.get(key) == current.get(key) for key in keys):
        return False
    if (accept_remount and 'volume_uuid' not in old and current.get('volume_uuid') and
            all(old[key] == current[key] for key in ('path', 'inode'))):
        return True
    raise ValueError('History database identity changed; review a legacy remount or use a new analysis directory')


def verify_remount_rows(db, previous):
    available = {row[1] for row in db.execute('PRAGMA table_info(events)')}
    columns = previous['columns']
    if not set(columns).issubset(set(COLUMNS + OPTIONAL_COLUMNS)) or not set(columns).issubset(available):
        raise ValueError('History remount continuity columns unavailable')
    matched = 0
    for row in previous['events']:
        current = db.execute('SELECT ' + ','.join(columns) + ' FROM events WHERE id=?', (row[0],)).fetchone()
        if current is not None:
            if list(current) != row:
                raise ValueError('History remount continuity mismatch')
            matched += 1
    if not matched:
        raise ValueError('History remount continuity requires retained matching rows')
    return matched


def capture(database, usage_state, kit, output, label, accept_history_remount=False):
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    if output.is_symlink() or output.stat().st_uid != os.getuid() or output.stat().st_mode & 0o077:
        raise ValueError('Output must be a private user-owned directory')
    with (output / '.capture.lock').open('a') as lock:
        os.chmod(lock.name, 0o600)
        fcntl.flock(lock, fcntl.LOCK_EX)
        return capture_locked(database, usage_state, kit, output, label, accept_history_remount)


def capture_locked(database, usage_state, kit, output, label, accept_history_remount):
    captures = sorted(output.glob('capture-*.json'))
    previous = json.loads(captures[-1].read_text()) if captures else None
    identity = database_identity(database)
    remount = previous is not None and review_identity(previous, identity, accept_history_remount)
    continuity_rows = 0
    manifest = json.loads((kit / 'shin-kit.json').read_text())
    usage = json.loads(usage_state.read_text())
    if usage.get('schema_version') != 'shin.model-usage.v3':
        raise ValueError('Unsupported numerical usage schema')
    if previous and previous['model_usage'].get('config_sha256') != usage.get('config_sha256'):
        raise ValueError('Usage mapping changed; use a new reviewed analysis directory')
    with closing(sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True, timeout=5)) as db:
        db.execute('BEGIN')
        if (db.execute('PRAGMA application_id').fetchone()[0] != 1397246280 or
                db.execute('PRAGMA user_version').fetchone()[0] != 2):
            raise ValueError('Unsupported numerical history schema')
        if remount:
            continuity_rows = verify_remount_rows(db, previous)
        low, high = db.execute('SELECT min(id),max(id) FROM events').fetchone()
        last = previous['through_id'] if previous else (high or 0)
        if previous and high is not None and high < last:
            raise ValueError('History sequence moved backward; use a new reviewed analysis directory')
        available = {row[1] for row in db.execute('PRAGMA table_info(events)')}
        columns = COLUMNS + [column for column in OPTIONAL_COLUMNS if column in available]
        rows = db.execute('SELECT ' + ','.join(columns) + ' FROM events WHERE id>? ORDER BY id LIMIT 20001', (last,)).fetchall()
        if len(rows) > 20000:
            raise ValueError('History exceeds the qualified row bound')
    if database_identity(database) != identity:
        raise ValueError('History database identity changed during capture')
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
    if remount:
        value['database_transition'] = {'reason': 'operator-reviewed-legacy-remount',
                                        'previous': previous['database'], 'retained_rows_matched': continuity_rows}
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
    parser.add_argument('--accept-history-remount', action='store_true',
                        help='Review one legacy device-number change; requires the same path/inode and matching retained rows')
    args = parser.parse_args()
    path, value = capture(args.home/'service/history/telemetry.sqlite', args.usage_state,
                          args.kit, args.out, args.label, args.accept_history_remount)
    print(json.dumps({'file': str(path), 'baseline': value['baseline'],
                      'new_rows': len(value['events']), 'through_id': value['through_id'],
                      'missing_rows_before_slice': value['missing_rows_before_slice']}))


if __name__ == '__main__':
    main()
