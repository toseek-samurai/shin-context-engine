import json
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
from capture_local_review import capture, COLUMNS, OPTIONAL_COLUMNS


class ReviewCaptureTest(unittest.TestCase):
    @patch('capture_local_review.time.time_ns', return_value=1791760000000000000)
    def test_incremental_read_only_export_and_retention_gap(self, _clock):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            database = base/'history.sqlite'
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('PRAGMA application_id=1397246280')
                db.execute('PRAGMA user_version=2')
                db.execute('CREATE TABLE events (' + ','.join('id INTEGER PRIMARY KEY' if c == 'id' else c for c in COLUMNS) + ')')
                db.execute("INSERT INTO events(id,kind,project) VALUES(1,'request','a')")
            usage = base/'usage.json';usage.write_text(json.dumps({'schema_version':'shin.model-usage.v3','buckets':[]}))
            (base/'shin-kit.json').write_text(json.dumps({'version':'test','core_version':'test'}))
            out = base/'out'
            _, first = capture(database, usage, base, out, 'start')
            self.assertTrue(first['baseline']);self.assertEqual(first['events'], [])
            with closing(sqlite3.connect(database)) as db, db:
                for column in OPTIONAL_COLUMNS:
                    db.execute(f'ALTER TABLE events ADD COLUMN {column}')
                db.execute('ALTER TABLE events ADD COLUMN private_payload')
                db.execute("INSERT INTO events(id,kind,project,session_id,private_payload) VALUES(2,'request','b',?,'DO-NOT-EXPORT')", ('a' * 64,))
            before = database.read_bytes()
            _, second = capture(database, usage, base, out, 'milestone')
            self.assertEqual(database.read_bytes(), before)
            self.assertEqual([r[0] for r in second['events']], [2])
            self.assertEqual(second['missing_rows_before_slice'], 0)
            self.assertEqual(second['columns'], COLUMNS + OPTIONAL_COLUMNS)
            self.assertEqual(second['events'][0][second['columns'].index('session_id')], 'a' * 64)
            self.assertNotIn('DO-NOT-EXPORT', json.dumps(second))
            _, unchanged = capture(database, usage, base, out, 'same')
            self.assertEqual(unchanged['events'], [])
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('DELETE FROM events');db.execute("INSERT INTO events(id,kind) VALUES(9,'sample')")
            _, gap = capture(database, usage, base, out, 'after-retention')
            self.assertEqual(gap['missing_rows_before_slice'], 6)
            self.assertEqual([r[0] for r in gap['events']], [9])
            self.assertEqual(gap['missing_rows_in_slice'], 6)
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("INSERT INTO events(id,kind) VALUES(12,'request')")
            _, interior = capture(database, usage, base, out, 'interior-retention-gap')
            self.assertEqual(interior['missing_rows_before_slice'], 0)
            self.assertEqual(interior['missing_rows_in_slice'], 2)
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('PRAGMA user_version=3')
            with self.assertRaisesRegex(ValueError, 'schema'):
                capture(database, usage, base, out, 'unsupported')
            self.assertEqual(len(list(out.glob('capture-*.json'))), 5)
            self.assertEqual([json.loads(p.read_text())['sequence'] for p in sorted(out.glob('capture-*.json'))], [1, 2, 3, 4, 5])

    def test_legacy_remount_review_and_stable_volume_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            database = base/'history.sqlite'
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('PRAGMA application_id=1397246280')
                db.execute('PRAGMA user_version=2')
                db.execute('CREATE TABLE events (' + ','.join('id INTEGER PRIMARY KEY' if c == 'id' else c for c in COLUMNS) + ')')
                db.execute("INSERT INTO events(id,kind) VALUES(1,'request')")
            usage = base/'usage.json'
            usage.write_text(json.dumps({'schema_version': 'shin.model-usage.v3'}))
            (base/'shin-kit.json').write_text(json.dumps({'version': 'test', 'core_version': 'test'}))
            out = base/'out'
            legacy = {'path': str(database), 'device': 100, 'inode': 42}
            def run(identity, label, accept=False):
                with patch('capture_local_review.database_identity', return_value=identity):
                    return capture(database, usage, base, out, label, accept)
            run(legacy, 'baseline')
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("INSERT INTO events(id,kind) VALUES(2,'request')")
            run(legacy, 'old-milestone')
            before = {p.name: p.read_bytes() for p in out.glob('capture-*.json')}
            remount = {**legacy, 'device': 200, 'volume_uuid': 'volume-a'}
            with self.assertRaisesRegex(ValueError, 'identity changed'):
                run(remount, 'unreviewed')
            for replacement in ({**remount, 'inode': 99}, {**remount, 'path': '/different'}):
                with self.assertRaisesRegex(ValueError, 'identity changed'):
                    run(replacement, 'replacement', True)
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("UPDATE events SET kind='sample' WHERE id=2")
            with self.assertRaisesRegex(ValueError, 'continuity mismatch'):
                run(remount, 'different-content', True)
            with closing(sqlite3.connect(database)) as db, db:
                db.execute('DELETE FROM events WHERE id=2')
            with self.assertRaisesRegex(ValueError, 'retained matching rows'):
                run(remount, 'no-continuity', True)
            with closing(sqlite3.connect(database)) as db, db:
                db.execute("INSERT INTO events(id,kind) VALUES(2,'request')")
                db.execute("INSERT INTO events(id,kind) VALUES(3,'request')")
            _, reviewed = run(remount, 'reviewed', True)
            self.assertFalse(reviewed['baseline'])
            self.assertEqual(reviewed['after_id'], 2)
            self.assertEqual([r[0] for r in reviewed['events']], [3])
            self.assertEqual(reviewed['database_transition']['retained_rows_matched'], 1)
            self.assertEqual({name: (out/name).read_bytes() for name in before}, before)
            _, after_boot = run({**remount, 'device': 300}, 'next-boot')
            self.assertEqual(after_boot['sequence'], 4)
            self.assertNotIn('database_transition', after_boot)
            with self.assertRaisesRegex(ValueError, 'identity changed'):
                run({**remount, 'volume_uuid': 'volume-b'}, 'different-volume', True)
            self.assertEqual(len(list(out.glob('capture-*.json'))), 4)


if __name__ == '__main__':
    unittest.main()
