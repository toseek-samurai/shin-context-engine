"""Pin refresh scenario coverage and the original shared resource budget."""
import contextlib
import io
import subprocess
import unittest
from unittest.mock import patch

import run_refresh_race_tests as harness


GROUPS = ('source-races', 'observations', 'documentation', 'safety')


class RefreshRaceHarnessTests(unittest.TestCase):
    def run_groups(self, clock, outcomes):
        with patch.object(harness, 'ThreadPoolExecutor') as factory, \
                patch.object(harness.time, 'monotonic', side_effect=clock), \
                patch.object(harness.subprocess, 'run', side_effect=outcomes) as run:
            pool = factory.return_value.__enter__.return_value
            # Execute in a controlled queue order, without sleeping or starting
            # native processes, so a late group's remaining budget is exact.
            pool.map.side_effect = lambda callback, groups: [callback(group) for group in groups]
            results = harness.run_groups('synthetic-runner', 'synthetic-profiles')
        return results, factory, run

    @staticmethod
    def successful_outcomes():
        return [subprocess.CompletedProcess([], 0, 'evidence for ' + group) for group in GROUPS]

    def test_groups_share_the_original_deadline_and_three_workers(self):
        results, factory, run = self.run_groups(
            [100, 100, 104, 112, 139], self.successful_outcomes())
        self.assertEqual(results, [(group, True, 'evidence for ' + group) for group in GROUPS])
        factory.assert_called_once_with(max_workers=3)
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ['synthetic-runner', 'synthetic-profiles', '--group', group] for group in GROUPS])
        self.assertEqual([call.kwargs['timeout'] for call in run.call_args_list], [40, 36, 28, 1])
        for call in run.call_args_list:
            self.assertEqual(call.kwargs['stdout'], subprocess.PIPE)
            self.assertEqual(call.kwargs['stderr'], subprocess.STDOUT)
            self.assertEqual(call.kwargs['encoding'], 'utf-8')
            self.assertEqual(call.kwargs['errors'], 'replace')

    def test_expired_groups_cannot_start_a_fresh_budget(self):
        results, factory, run = self.run_groups(
            [100, 100, 110, 141, 160], self.successful_outcomes())
        self.assertEqual([row[0] for row in results], list(GROUPS))
        self.assertEqual([call.kwargs['timeout'] for call in run.call_args_list], [40, 30, .001, .001])
        factory.assert_called_once_with(max_workers=3)

    def test_nonzero_exit_keeps_failed_group_diagnostics(self):
        outcomes = self.successful_outcomes()
        outcomes[0] = subprocess.CompletedProcess([], 7, 'independent source fence refused\n')
        results, _, run = self.run_groups([100] * 5, outcomes)
        self.assertEqual(results[0], ('source-races', False, 'independent source fence refused\n'))
        self.assertEqual(results[1:], [(group, True, 'evidence for ' + group) for group in GROUPS[1:]])
        self.assertEqual(run.call_count, 4)

    def test_timeout_keeps_decoded_partial_output_and_fails(self):
        for partial, expected in ((b'stalled \xff', 'stalled \ufffd'),
                                  ('observation stalled', 'observation stalled'), (None, '')):
            with self.subTest(partial=partial):
                outcomes = self.successful_outcomes()
                outcomes[0] = subprocess.TimeoutExpired(['synthetic-runner'], 40, output=partial)
                results, _, run = self.run_groups([100] * 5, outcomes)
                self.assertEqual(results[0], ('source-races', False,
                    expected + '\nShared refresh test deadline exceeded.\n'))
                self.assertEqual([row[0] for row in results], list(GROUPS))
                self.assertTrue(all(row[1] for row in results[1:]))
                self.assertEqual(run.call_count, 4)

    def test_unexpected_process_start_failure_is_not_hidden(self):
        failure = PermissionError('independent process launch refusal')
        with self.assertRaises(PermissionError) as caught:
            self.run_groups([100, 100], [failure])
        self.assertIs(caught.exception, failure)

    def test_main_reports_every_successful_group(self):
        output = io.StringIO()
        rows = [(group, True, 'successful diagnostic stays compact') for group in GROUPS]
        with patch.object(harness, 'run_groups', return_value=rows) as run, \
                patch.object(harness.sys, 'argv', ['runner.py', 'synthetic-runner', 'synthetic-profiles']), \
                contextlib.redirect_stdout(output):
            self.assertEqual(harness.main(), 0)
        run.assert_called_once_with('synthetic-runner', 'synthetic-profiles')
        self.assertEqual(output.getvalue(), ''.join(group + ': passed\n' for group in GROUPS))

    def test_main_fails_any_group_and_prints_its_diagnostics(self):
        for failed in ({'source-races'}, {'safety'}, {'observations', 'documentation'}):
            with self.subTest(failed=failed):
                rows = [(group, group not in failed, 'diagnostic for ' + group) for group in GROUPS]
                output = io.StringIO()
                with patch.object(harness, 'run_groups', return_value=rows), \
                        patch.object(harness.sys, 'argv', ['runner.py', 'runner', 'profiles']), \
                        contextlib.redirect_stdout(output):
                    self.assertEqual(harness.main(), 1)
                text = output.getvalue()
                for group in GROUPS:
                    self.assertIn(group + (': FAILED\n' if group in failed else ': passed\n'), text)
                    self.assertEqual(text.count('diagnostic for ' + group), 1 if group in failed else 0)


if __name__ == '__main__':
    unittest.main()
