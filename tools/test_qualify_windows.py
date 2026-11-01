"""Portable negative checks for the disposable Windows harness cleanup."""
import unittest
from unittest.mock import Mock, patch

from qualify_windows import cleanup_temporary


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


if __name__ == '__main__':
    unittest.main()
