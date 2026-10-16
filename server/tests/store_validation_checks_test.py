#!/usr/bin/env python3
"""Pure contract checks for the Store validation sidecar."""

from __future__ import annotations

import unittest

from store_validation_checks import DOMAIN_PROBES, SCHEMA_UNSUPPORTED, _assert_failure_code, probe_column


class StoreValidationChecksTest(unittest.TestCase):
    def test_each_domain_has_a_distinct_sql_safe_probe_column(self) -> None:
        domains = [domain for domain, _ in DOMAIN_PROBES]
        columns = [probe_column(domain) for domain in domains]

        self.assertEqual(
            domains,
            ["activation", "workspace", "observer", "audit", "recovery", "identity", "base-preparations",
             "publication-proposals", "publication-resolutions", "publication-heads", "publication-withdrawals",
             "workspace-publication-history"],
        )
        self.assertEqual(len(columns), len(set(columns)))
        self.assertTrue(all(column.startswith("p3e10_store_validation_probe_") for column in columns))
        self.assertTrue(all(column.replace("_", "").isalnum() for column in columns))

    def test_unknown_domain_is_rejected_before_sql_is_formed(self) -> None:
        with self.assertRaises(ValueError):
            probe_column("not-a-domain")

    def test_bad_or_missing_error_code_cannot_pass(self) -> None:
        _assert_failure_code({"error": {"code": "schema-unsupported"}}, SCHEMA_UNSUPPORTED, "fixture")
        for response in ({}, {"error": None}, {"error": {"code": "database-unavailable"}}):
            with self.subTest(response=response), self.assertRaises(RuntimeError):
                _assert_failure_code(response, SCHEMA_UNSUPPORTED, "fixture")


if __name__ == "__main__":
    unittest.main()
