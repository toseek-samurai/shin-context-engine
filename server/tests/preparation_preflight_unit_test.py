"""Keep the HTTP preflight oracle strict about independent resource counters."""

import copy
import unittest

from integration_test import IntegrationFailure
from preparation_preflight_checks import PreparationPreflightChecks


class ResourceObservationTests(unittest.TestCase):
    def setUp(self):
        self.checks = PreparationPreflightChecks()
        self.row = {"state": "failed", "total": 1, "passed": 0, "failed": 1,
                    "query_total": 1, "query_passed": 0,
                    "resource_check": {"policy": "strict", "ok": False,
                                       "failed_queries": 1, "warning_queries": 0}}

    def test_strict_resource_failure_is_a_valid_observation(self):
        self.checks._assert_suite(self.row, state="failed", resource_failed_queries=1)

    def test_advisory_warning_does_not_imply_quality_pass(self):
        for state in ("passed", "failed"):
            row = copy.deepcopy(self.row)
            row.update(state=state, passed=int(state == "passed"), failed=int(state == "failed"),
                       query_passed=int(state == "passed"))
            row["resource_check"].update(policy="advisory", warning_queries=1)
            self.checks._assert_suite(row, state=state, resource_policy="advisory", resource_failed_queries=1)

    def test_forged_resource_observations_are_rejected(self):
        for patch in ({"ok": True}, {"failed_queries": 0}, {"warning_queries": 1},
                      {"policy": "advisory"}, {"failed_queries": True}, {"ok": 0}):
            with self.subTest(patch=patch):
                row = copy.deepcopy(self.row)
                row["resource_check"].update(patch)
                with self.assertRaises(IntegrationFailure):
                    self.checks._assert_suite(row, state="failed", resource_failed_queries=1)

    def test_empty_eval_is_not_a_resource_pass(self):
        row = {key: 0 for key in ("total", "passed", "failed", "query_total", "query_passed")}
        row.update(state="not-configured", resource_check={
            "policy": "strict", "ok": False, "failed_queries": 0, "warning_queries": 0})
        self.checks._assert_suite(row, state="not-configured")

    def test_pack_and_unassessed_keep_resource_observation_null(self):
        row = copy.deepcopy(self.row)
        row.update(query_total=0, query_passed=0, resource_check=None)
        self.checks._assert_suite(row, state="failed", pack_query_count_is_zero=True)
        row = {key: None for key in row}
        row["state"] = "not-assessed"
        self.checks._assert_suite(row, state="not-assessed")


if __name__ == "__main__":
    unittest.main()
