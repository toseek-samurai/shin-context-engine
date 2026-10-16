import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from codex_patch_guard import isolated_command
from pilot_tools import validate_tool
from pilot_comparison_query import verified_records, bounded_result, search


class ChoiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='shin-choice-unit-', dir='/private/tmp')
        self.directory = Path(self.temp.name).resolve()
        self.root = self.directory / 'root'
        (self.root / 'src').mkdir(parents=True)
        self.source = self.root / 'src/probe.cpp'
        self.source.write_text('// first\nint probe() { return 42; }\n')
        self.hashes = {'src/probe.cpp': hashlib.sha256(self.source.read_bytes()).hexdigest()}

    def tearDown(self):
        self.temp.cleanup()

    def test_choice_tools_only_when_admitted(self):
        state = {'comparison': {'providers': ['jbcontext']}}
        self.assertIn('jbcontext', validate_tool(state, 'jbcontext_search', {'query': 'a function'}))
        for name, args in [('shin_search', {'query': 'x'}), ('jbcontext_search', {'query': ''}),
                           ('jbcontext_search', {'query': 'x', 'revision': 'foreign'})]:
            with self.assertRaises(ValueError):
                validate_tool(state, name, args)

    def test_shell_cannot_edit_readonly_source(self):
        result = subprocess.run(['/bin/bash', '-c', isolated_command(self.root, 'touch forbidden', True)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'forbidden').exists())

    def test_verified_source_text_and_coordinates(self):
        rows = verified_records(self.root, self.hashes, [{'path': 'src/probe.cpp', 'text': 'int probe() { return 42; }', 'line': 2}])
        self.assertEqual(rows[0]['line'], 2)
        for row in [{'path': '../secret', 'text': 'secret'}, {'path': 'src/probe.cpp', 'text': 'wrong'},
                    {'path': 'src/probe.cpp', 'text': 'int probe()', 'line': 1}]:
            with self.assertRaises(ValueError):
                verified_records(self.root, self.hashes, [row])

    def test_whole_records_are_omitted_not_silently_clipped(self):
        result = json.loads(bounded_result('shin', [{'text': 'x' * 9000}], 8192))
        self.assertEqual(result['results'], [])
        self.assertEqual(result['omitted'], 1)

    def test_remote_revision_and_exact_content_are_required(self):
        state_path = self.directory / 'state.json'
        config = {'providers': ['jbcontext'], 'source_hashes': self.hashes, 'revision': 'frozen',
                  'repository': 'test', 'jbcontext': '/unused', 'credential_home': '/unused'}
        state_path.write_text(json.dumps({'root': str(self.root), 'comparison': config}))
        row = {'result': {'sourcePosition': {'relativePath': 'src/probe.cpp'}},
               'content': 'int probe() { return 42; }', 'contentStartLine': 2}
        for revision in ['wrong', None, 'frozen']:
            response = subprocess.CompletedProcess([], 0, json.dumps({'revision': revision, 'results': [row]}).encode(), b'')
            with patch('pilot_comparison_query.subprocess.run', return_value=response):
                if revision != 'frozen':
                    with self.assertRaises(ValueError):
                        search(state_path, 'jbcontext', 'probe', 8192)
                else:
                    result = json.loads(search(state_path, 'jbcontext', 'probe', 8192))
                    self.assertTrue(result['source_verified'])
        self.source.write_text('changed')
        with self.assertRaises(ValueError):
            search(state_path, 'jbcontext', 'probe', 8192)


if __name__ == '__main__':
    unittest.main()
