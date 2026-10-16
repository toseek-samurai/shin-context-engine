#!/usr/bin/env python3
"""Native numerical collector scheduling: no real client histories or launchd writes."""
import datetime
import sqlite3
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(sys.argv.pop(1)).resolve()


class UsageCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='shin-usage-cli-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.config = self.base / 'usage & config.json'
        self.home = self.base / 'history <private>'
        self.data = {'schema_version': 'shin.usage-collector.v1',
                     'projects': [{'id': 'p', 'name': 'Project', 'paths': ['/project']}],
                     'sources': {client: {'database': str(self.base / f'{client}.sqlite')}
                                 for client in ('codex', 'opencode', 'hermes')}}
        self.save_config()

    def save_config(self):
        self.config.write_text(json.dumps(self.data))
        self.config.chmod(0o600)

    def run_usage(self, *args, expected=0):
        run = subprocess.run([str(TOOL), '--config', str(self.config), '--home', str(self.home), *args],
                             capture_output=True, text=True, timeout=15, env={**os.environ, 'PATH': ''})
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        return json.loads(run.stdout if expected == 0 else run.stderr)

    def codex_fixture(self, mutate=None):
        sessions, archives = self.base / 'sessions', self.base / 'archives'
        sessions.mkdir(); archives.mkdir()
        path = sessions / 'rollout.jsonl'
        now = datetime.datetime.now(datetime.timezone.utc)
        usage = {'input_tokens': 100, 'output_tokens': 20, 'cached_input_tokens': 60}
        token = {'timestamp': now.isoformat().replace('+00:00', 'Z'), 'type': 'event_msg',
                 'payload': {'type': 'token_count', 'info': {'total_token_usage': usage, 'last_token_usage': usage}}}
        path.write_text(json.dumps({'type': 'session_meta', 'payload': {'id': 'one'}}) + '\n' +
                        json.dumps({'type': 'turn_context', 'payload': {'model': 'fixture-model'}}) + '\n')
        path.chmod(0o600)  # Safe fixture regardless of the invoking shell's umask.
        if mutate:
            path = mutate(path)
        if path.exists() and path.stat().st_size <= 2 * 1024**3:
            with path.open('a') as out:
                out.write(json.dumps(token) + '\n' + json.dumps(token) + '\n')
        db = self.base / 'codex.sqlite'
        with sqlite3.connect(db) as c:
            c.execute('CREATE TABLE threads(id,rollout_path,cwd,created_at_ms,created_at,updated_at_ms,updated_at)')
            c.execute('INSERT INTO threads VALUES(?,?,?,?,?,?,?)',
                      ('one', str(path), '/project', 0, 0, int(now.timestamp() * 1000), 0))
        self.data['sources']['codex'].update(database=str(db), sessions=str(sessions), archives=str(archives))
        self.save_config()

    def test_large_session_within_sweep_budget_is_counted_once(self):
        def large(path):
            with path.open('r+b') as out:
                out.seek(1024**3 + 32)
                out.write(b'\n')
            return path
        self.codex_fixture(large)
        result = self.run_usage()
        self.assertEqual(result['sources']['codex']['skipped_sessions'], 0)
        self.assertEqual(result['sources']['codex']['source_records'], 1)
        # The oversized non-numeric line stays explicitly incomplete; token events are deduplicated.
        self.assertEqual(result['sources']['codex']['state'], 'partial')
        self.assertEqual(result['sources']['codex']['skipped_reasons'], {'rollout-lines-omitted': 1})
        self.assertEqual(json.loads((self.home / 'state.json').read_text())['buckets'][0]['input_tokens'], 100)

    def test_missing_history_has_specific_reason(self):
        def missing(path):
            path.unlink()
            return path
        self.codex_fixture(missing)
        result = self.run_usage()
        self.assertEqual(result['sources']['codex']['skipped_reasons'], {'rollout-missing': 1})
        self.assertEqual(result['buckets'], 0)

    def test_oversized_history_is_rejected_before_reading(self):
        def too_large(path):
            with path.open('r+b') as out:
                out.truncate(4 * 1024**3 + 1)
            return path
        self.codex_fixture(too_large)
        result = self.run_usage()
        self.assertEqual(result['sources']['codex']['skipped_reasons'], {'rollout-too-large': 1})
        self.assertEqual(result['buckets'], 0)

    def test_total_read_budget_is_not_expanded(self):
        self.codex_fixture()
        path = self.base / 'sessions/second.jsonl'
        with path.open('wb') as out:
            out.truncate(4 * 1024**3)
        path.chmod(0o600)
        with sqlite3.connect(self.base / 'codex.sqlite') as c:
            c.execute("INSERT INTO threads SELECT 'two',?,'/project',0,0,updated_at_ms-1,0 FROM threads LIMIT 1", (str(path),))
        result = self.run_usage()
        self.assertEqual(result['sources']['codex']['skipped_reasons'], {'usage-byte-budget-exhausted': 1})
        self.assertEqual(result['sources']['codex']['source_records'], 1)
        self.assertEqual(json.loads((self.home / 'state.json').read_text())['buckets'][0]['input_tokens'], 100)

    def test_writable_history_remains_rejected(self):
        def unsafe(path):
            path.chmod(0o666)
            return path
        self.codex_fixture(unsafe)
        result = self.run_usage()
        self.assertEqual(result['sources']['codex']['skipped_reasons'], {'rollout-unsafe-file': 1})
        self.assertEqual(result['buckets'], 0)

    def test_configuration_pin_and_unavailable_are_explicit(self):
        result = self.run_usage()
        self.assertEqual({s['state'] for s in result['sources'].values()}, {'unavailable'})
        old = (self.home / 'state.json').read_bytes()
        self.data['projects'][0]['paths'] = ['/other-project']
        self.save_config()
        self.assertEqual(self.run_usage(expected=2)['error'], 'usage-config-changed')
        self.assertEqual(old, (self.home / 'state.json').read_bytes())

    def test_invalid_options_are_not_accepted(self):
        self.assertEqual(self.run_usage('--watch', '--service-definition', expected=2)['error'], 'usage-arguments')
        self.assertEqual(self.run_usage('--watch', '--watch', expected=2)['error'], 'usage-arguments')
        self.assertFalse(self.home.exists())

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS launchd definition')
    def test_definition_is_read_only_and_runs_bounded_one_shots(self):
        self.run_usage('--service-definition', expected=2)
        self.assertFalse(self.home.exists())
        self.run_usage()
        before = {p.name: p.read_bytes() for p in self.home.iterdir() if p.is_file()}
        definition = self.run_usage('--service-definition')
        self.assertFalse(definition['installation_performed'])
        self.assertFalse(definition['collection_performed'])
        unit = plistlib.loads(definition['plist'].encode())
        self.assertEqual(unit, definition['unit'])
        self.assertEqual(unit['ProgramArguments'], [str(TOOL), '--config', str(self.config), '--home', str(self.home)])
        self.assertEqual(unit['StartInterval'], 300)
        self.assertEqual(unit['Umask'], 0o77)
        self.assertTrue(unit['RunAtLoad'])
        self.assertNotIn('KeepAlive', unit)
        self.assertNotIn('--watch', unit['ProgramArguments'])
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.home.iterdir() if p.is_file()})
        self.assertEqual(definition, self.run_usage('--service-definition'))
        self.data['projects'][0]['name'] = 'Changed'
        self.save_config()
        self.assertEqual(self.run_usage('--service-definition', expected=2)['error'], 'usage-config-changed')


if __name__ == '__main__':
    unittest.main()
