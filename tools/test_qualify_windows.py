"""Portable checks for Windows qualification receipts and disposable cleanup."""
import unittest
import json
import tempfile
from pathlib import Path
from unittest.mock import Mock, patch

from qualify_windows import cleanup_temporary, qualification_receipt


def denied(code):
    error = PermissionError('independent synthetic cleanup failure')
    error.winerror = code
    return error


class CleanupTests(unittest.TestCase):
    def test_owned_sharing_lock_can_settle(self):
        for code in (32, 33):
            with self.subTest(code=code):
                temporary = Mock()
                temporary.cleanup.side_effect = [denied(code), None]
                with patch('qualify_windows.time.monotonic', side_effect=[0, 0.1]), \
                        patch('qualify_windows.time.sleep') as sleep:
                    cleanup_temporary(temporary)
                self.assertEqual(temporary.cleanup.call_count, 2)
                sleep.assert_called_once_with(0.05)

    def test_acl_failure_is_not_retried_or_ignored(self):
        temporary = Mock()
        error = denied(5)
        temporary.cleanup.side_effect = error
        with patch('qualify_windows.time.sleep') as sleep:
            with self.assertRaises(PermissionError) as caught:
                cleanup_temporary(temporary)
        self.assertIs(caught.exception, error)
        temporary.cleanup.assert_called_once()
        sleep.assert_not_called()

    def test_leaked_image_lock_still_fails(self):
        temporary = Mock()
        error = denied(32)
        temporary.cleanup.side_effect = error
        with patch('qualify_windows.time.monotonic', side_effect=[0, 6]), \
                patch('qualify_windows.time.sleep') as sleep:
            with self.assertRaises(PermissionError) as caught:
                cleanup_temporary(temporary)
        self.assertIs(caught.exception, error)
        temporary.cleanup.assert_called_once()
        sleep.assert_not_called()


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='shin-windows-receipt-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'observer').mkdir()
        (self.root / 'VERSION').write_text('2.0.5-dev.13\n')
        (self.root / 'observer/VERSION').write_text('2.0.5-dev.91\n')
        self.identity = dict(source_sha='a' * 40, run_id=901, attempt=2,
                             assets={'synthetic.msi': 'sha256:' + '1' * 64})
        self.metadata = dict(self.identity, version='2.0.5-dev.91', core_version='2.0.5-dev.13')
        self.runtime = dict(self.identity, unelevated=True, render_scope='headless')
        self.installed = dict(self.metadata, unelevated=True, msi_root_verified=True, helper_staged=True)
        self.write_reports()

    def write_reports(self):
        for name, value in (('package', self.metadata), ('runtime', self.runtime), ('installed-preflight', self.installed)):
            (self.root / (name + '.json')).write_text(json.dumps(value))

    def receipt(self):
        with patch('qualify_windows.ROOT', self.root):
            return qualification_receipt(self.root, self.identity)

    def test_complete_installed_proof_is_bound_to_the_exact_package(self):
        receipt = self.receipt()
        self.assertEqual(receipt['source_sha'], self.identity['source_sha'])
        self.assertEqual(receipt['assets'], self.identity['assets'])
        self.assertEqual(receipt['version'], '2.0.5-dev.91')
        self.assertEqual(receipt['core_version'], '2.0.5-dev.13')
        self.assertIn('unelevated-runtime', receipt['checks'])
        self.assertIn('installed-update-preflight', receipt['checks'])

    def test_missing_installed_proof_cannot_issue_a_receipt(self):
        (self.root / 'installed-preflight.json').unlink()
        with self.assertRaises(FileNotFoundError):
            self.receipt()

    def test_wrong_or_unverified_installed_proof_cannot_issue_a_receipt(self):
        failures = dict(source_sha='b' * 40, run_id=902, attempt=1,
                        assets={'synthetic.msi': 'sha256:' + '2' * 64},
                        version='2.0.5-dev.90', core_version='2.0.5-dev.12',
                        unelevated=False, msi_root_verified=False, helper_staged=False)
        for key, value in failures.items():
            with self.subTest(field=key):
                changed = dict(self.installed, **{key: value})
                (self.root / 'installed-preflight.json').write_text(json.dumps(changed))
                with self.assertRaises(AssertionError):
                    self.receipt()


if __name__ == '__main__':
    unittest.main()
