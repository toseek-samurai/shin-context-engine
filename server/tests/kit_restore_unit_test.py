#!/usr/bin/env python3
"""Admission and cleanup boundaries for the isolated matched-kit rehearsal."""
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

from headless_workflow_test import IntegrationFailure
from kit_restore_test import CandidateTrial, connection_value, database_fingerprint, validate_components


def components():
    return {
        "shin-context": {"envelope": {"tool": "shin-context", "ok": True, "version": "core"}},
        "shin-server": {"envelope": {"tool": "shin-server", "ok": True,
            "result": {"core_version": "core", "contracts": {"configuration_schema": "shin.server-config.v10"}}}},
        "shin-observer": {"envelope": {"tool": "shin-observer", "ok": True, "core_version": "core"}}}


class KitRestoreTests(unittest.TestCase):
    def test_matched_components(self):
        validate_components(components())

    def test_incomplete_kit(self):
        value = components()
        del value["shin-observer"]
        with self.assertRaises(IntegrationFailure):
            validate_components(value)

    def test_mixed_core_is_rejected(self):
        for name in ("shin-server", "shin-observer"):
            value = components()
            envelope = value[name]["envelope"]
            (envelope["result"] if name == "shin-server" else envelope)["core_version"] = "different"
            with self.subTest(name=name), self.assertRaises(IntegrationFailure):
                validate_components(value)

    def test_unknown_configuration_schema(self):
        value = components()
        value["shin-server"]["envelope"]["result"]["contracts"]["configuration_schema"] = "unknown"
        with self.assertRaises(IntegrationFailure):
            validate_components(value)

    def test_connection_rejects_injection(self):
        valid = ("shin_candidate_" + "a" * 16, "runtime", "b" * 32)
        self.assertIn("dbname=shin_candidate_", connection_value(*valid))
        for index, bad in ((0, "shin_server_dev"), (1, "postgres"), (2, "x\npassword=other")):
            args = list(valid)
            args[index] = bad
            with self.subTest(index=index), self.assertRaises(IntegrationFailure):
                connection_value(*args)

    def test_cleanup_never_drops_uncreated_or_foreign_database(self):
        suite = SimpleNamespace(compose=Mock())
        trial = CandidateTrial({})
        trial.cleanup(suite)
        suite.compose.assert_not_called()
        trial.created = True
        trial.database = "shin_server_dev"
        with self.assertRaises(IntegrationFailure):
            trial.cleanup(suite)
        suite.compose.assert_not_called()

    def test_cleanup_drops_only_owned_database_once(self):
        suite = SimpleNamespace(compose=Mock())
        trial = CandidateTrial({})
        trial.created = True
        trial.cleanup(suite)
        trial.cleanup(suite)
        suite.compose.assert_called_once_with("exec", "-T", "postgres", "dropdb", "-U", "shin_migrator", trial.database)

    def test_fingerprint_normalizes_only_postgres_restriction_token(self):
        suite = SimpleNamespace(compose=Mock())
        suite.compose.return_value = SimpleNamespace(stdout="\\restrict abc\nDATA\n\\unrestrict abc\n")
        first = database_fingerprint(suite, "shin_candidate_" + "a" * 16)
        suite.compose.return_value.stdout = "\\restrict xyz\nDATA\n\\unrestrict xyz\n"
        self.assertEqual(first, database_fingerprint(suite, "shin_candidate_" + "a" * 16))
        suite.compose.return_value.stdout = "\\restrict xyz\nCHANGED\n\\unrestrict xyz\n"
        self.assertNotEqual(first, database_fingerprint(suite, "shin_candidate_" + "a" * 16))


if __name__ == "__main__":
    unittest.main()
