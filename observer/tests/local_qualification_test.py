#!/usr/bin/env python3
"""Read-only cold admission in disposable native workers, without launchd."""
import json
import subprocess
import sys
import unittest
from pathlib import Path

DRIVER = Path(sys.argv.pop(3)).resolve()

import local_project_test as fixtures


class QualificationTests(fixtures.LocalProjectTests):
    def qualify(self, roots=None, expected=0, executable=None, cancel=False, wrong_identity=False):
        run = subprocess.run([str(DRIVER), str(executable or fixtures.TOOL), str(self.home),
                              *(['--cancel'] if cancel else []),
                              *(['--wrong-identity'] if wrong_identity else []),
                              *map(str, roots or [self.root])], capture_output=True, text=True, timeout=45)
        self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
        return json.loads(run.stdout)

    def test_repeated_admission_preserves_root_and_bound_state(self):
        self.init()
        root_before = self.file_map(self.root)
        home_before = self.file_map(self.home)
        result = self.qualify([self.root] * 3)
        self.assertEqual(len(result['results']), 3)
        for item in result['results']:
            self.assertTrue(item['ok'])
            self.assertTrue(item['local_ready'])
        self.assertEqual(root_before, self.file_map(self.root))
        self.assertEqual(home_before, self.file_map(self.home))

    def test_stale_generation_is_not_admitted_or_rebuilt(self):
        self.init()
        (self.root / 'README.md').write_text('# Changed after publication\n')
        before = self.file_map(self.home)
        result = self.qualify()
        self.assertFalse(result['results'][0]['local_ready'])
        self.assertEqual(before, self.file_map(self.home))

    def test_child_failure_is_not_readiness(self):
        self.init()
        result = self.qualify(expected=2, executable=Path('/usr/bin/false'))
        self.assertEqual(result['code'], 'service-disconnected')

    def test_cancel_reaps_worker_without_modifying_state(self):
        self.init()
        before = self.file_map(self.home)
        result = self.qualify(expected=2, cancel=True)
        self.assertEqual(result['code'], 'service-request-cancelled')
        self.assertEqual(before, self.file_map(self.home))

    def test_enrollment_identity_is_not_replaced_by_current_directory(self):
        self.init()
        before = self.file_map(self.home)
        result = self.qualify(expected=2, wrong_identity=True)
        self.assertEqual(result['code'], 'refresh-root-changed')
        self.assertEqual(before, self.file_map(self.home))


if __name__ == '__main__':
    # Reuse setup/helpers without repeating the full project lifecycle suite.
    suite = unittest.TestSuite(QualificationTests(name) for name in QualificationTests.__dict__
                               if name.startswith('test_'))
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
