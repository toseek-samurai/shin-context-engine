"""Strict public publication oracles, independently testable without a stand."""

from __future__ import annotations

from datetime import datetime
import copy
import json
import re
import unittest
from typing import Any

from integration_test import IntegrationFailure


IDENTITY_FIELDS = {"state", "revision", "preparation_id", "snapshot_id", "source_route_id", "workspace_id"}
SOURCE_METADATA_FIELDS = {"source_host_id", "profile_id"}
HEAD_FIELDS = IDENTITY_FIELDS | {"project_id", "publication_id", "history_epoch"}
PROPOSAL_FIELDS = {
    "id", "project_id", "publication_id", "preparation_id", "base_revision",
    "base_preparation_id", "base_withdrawal_id", "submitted_by", "created_at", "state",
}
CANDIDATE_FIELDS = {
    "preparation_id", "snapshot_id", "source_route_id", "workspace_id", "candidate_author", "state",
}
RECEIPT_FIELDS = {
    "proposal_id", "project_id", "publication_id", "decision", "expected_revision",
    "revision", "preparation_id", "created_at", "resolved_by",
}
TERMINAL_STATES = {"accepted", "rejected", "request-reprepare"}
WITHDRAWAL_RECEIPT_FIELDS = {
    "schema_version", "id", "project_id", "publication_id", "expected_revision", "revision",
    "previous_proposal_id", "previous_preparation_id", "withdrawn_by", "created_at",
}
SNAPSHOT = re.compile(r"fnv1a64:[0-9a-f]{16}\Z")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IntegrationFailure(message)


def exact_object(value: Any, fields: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict) and set(value) == fields, label + " public fields differ")
    return value


def source_object(value: Any, fields: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict) and fields <= set(value) <= fields | SOURCE_METADATA_FIELDS,
            label + " public source fields differ")
    return value


def integer(value: Any, minimum: int, label: str) -> None:
    require(type(value) is int and minimum <= value <= 9223372036854775807, label + " is not a bounded integer")


def timestamp(value: Any) -> None:
    require(isinstance(value, str) and value.endswith("Z"), "timestamp is not UTC")
    try:
        datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise IntegrationFailure("invalid publication timestamp") from error


def identity(value: Any) -> dict[str, Any]:
    result = source_object(value, IDENTITY_FIELDS, "publication identity")
    integer(result["revision"], 0, "revision")
    populated = [result[key] is not None for key in IDENTITY_FIELDS - {"revision", "state"}]
    require(all(populated) or not any(populated), "publication identity is only partially populated")
    require(result["state"] in {"empty", "active", "withdrawn"}, "invalid publication head state")
    require((result["state"] == "empty" and result["revision"] == 0 and not any(populated)) or
            (result["state"] == "active" and result["revision"] > 0 and all(populated)) or
            (result["state"] == "withdrawn" and result["revision"] > 0 and not any(populated)),
            "publication state, revision and selection disagree")
    if result["preparation_id"] is not None:
        integer(result["preparation_id"], 1, "preparation id")
        require(isinstance(result["snapshot_id"], str) and bool(SNAPSHOT.fullmatch(result["snapshot_id"])),
                "publication snapshot id is malformed")
        for field in ("source_route_id", "workspace_id"):
            require(isinstance(result[field], str) and bool(result[field]), "missing publication " + field)
    for field in SOURCE_METADATA_FIELDS & result.keys():
        require(result[field] is None if result["preparation_id"] is None else isinstance(result[field], str) and bool(result[field]),
                "publication source metadata is only partially populated")
    return result


def head(value: Any, project: str, publication: str) -> dict[str, Any]:
    result = exact_object(value, HEAD_FIELDS, "publication head")
    require(result["project_id"] == project and result["publication_id"] == publication,
            "publication head crossed its namespace")
    identity({key: result[key] for key in IDENTITY_FIELDS})
    integer(result["history_epoch"], 0, "publication admission epoch")
    return result


def detail(value: Any, project: str, publication: str) -> dict[str, Any]:
    result = exact_object(value, {"proposal", "base", "current", "candidate", "resolution"}, "proposal detail")
    proposal = exact_object(result["proposal"], PROPOSAL_FIELDS, "proposal")
    require(proposal["project_id"] == project and proposal["publication_id"] == publication,
            "proposal crossed its namespace")
    integer(proposal["id"], 1, "proposal id")
    integer(proposal["preparation_id"], 1, "candidate id")
    integer(proposal["base_revision"], 0, "base revision")
    timestamp(proposal["created_at"])
    require(isinstance(proposal["submitted_by"], str) and bool(proposal["submitted_by"]), "missing proposal author")
    for side in ("base", "current"):
        exact_object(result[side], IDENTITY_FIELDS | SOURCE_METADATA_FIELDS, side + " provenance")
    base, current = identity(result["base"]), identity(result["current"])
    require(base["revision"] == proposal["base_revision"] and
            base["preparation_id"] == proposal["base_preparation_id"], "captured proposal base differs")
    if base["state"] == "withdrawn":
        integer(proposal["base_withdrawal_id"], 1, "base withdrawal id")
    else:
        require(proposal["base_withdrawal_id"] is None, "non-withdrawn base has a withdrawal identity")
    candidate = exact_object(result["candidate"], CANDIDATE_FIELDS | SOURCE_METADATA_FIELDS, "proposal candidate")
    require(candidate["preparation_id"] == proposal["preparation_id"], "proposal candidate differs")
    require(isinstance(candidate["candidate_author"], str) and bool(candidate["candidate_author"]), "missing candidate author")
    require(candidate["state"] in {"queued", "running", "prepared", "failed", "cancelled"}, "invalid candidate state")
    identity({"revision": 1, "state": "active", **{key: candidate[key] for key in
              ((IDENTITY_FIELDS | SOURCE_METADATA_FIELDS) - {"state"}) & candidate.keys()}})
    if result["resolution"] is None:
        expected = "pending" if base["revision"] == current["revision"] else "conflicted"
        require(proposal["state"] == expected, "unresolved conflict state does not reflect the shared head")
    else:
        receipt = exact_object(result["resolution"], RECEIPT_FIELDS, "resolution")
        require(proposal["state"] in TERMINAL_STATES, "resolved proposal is not terminal")
        require(receipt["proposal_id"] == proposal["id"] and receipt["project_id"] == project and
                receipt["publication_id"] == publication, "resolution crossed its proposal identity")
        expected_state = {"accept": "accepted", "reject": "rejected", "request-reprepare": "request-reprepare"}
        require(proposal["state"] == expected_state.get(receipt["decision"]), "terminal state differs from its decision")
        timestamp(receipt["created_at"])
    return result


def withdrawal_receipt(value: Any, project: str, publication: str, revision: int,
                       proposal_id: int, preparation_id: int) -> dict[str, Any]:
    result = exact_object(value, WITHDRAWAL_RECEIPT_FIELDS, "withdrawal receipt")
    require(result["schema_version"] == "shin.publication-withdrawal-receipt.v1", "withdrawal schema differs")
    for field in ("id", "previous_proposal_id", "previous_preparation_id", "revision"):
        integer(result[field], 1, "withdrawal " + field)
    integer(result["expected_revision"], 0, "withdrawal expected revision")
    require(result["project_id"] == project and result["publication_id"] == publication and
            result["expected_revision"] == revision and result["revision"] == revision + 1 and
            result["previous_proposal_id"] == proposal_id and result["previous_preparation_id"] == preparation_id and
            result["withdrawn_by"] == "dev-admin", "withdrawal receipt lost its exact transition")
    timestamp(result["created_at"])
    return result


def no_private_values(value: Any, forbidden: list[str]) -> None:
    rendered = json.dumps(value, sort_keys=True)
    require(not any(item and item in rendered for item in forbidden), "publication response exposed private fixture data")


class PublicationOracleTests(unittest.TestCase):
    """These validate test helpers only; they are not HTTP or PostgreSQL evidence."""

    def test_revision_rejects_boolean_negative_and_overflow(self) -> None:
        empty = {**dict.fromkeys(IDENTITY_FIELDS), "state": "empty"}
        for value in (True, -1, "0", 9223372036854775808):
            with self.subTest(value=value), self.assertRaises(IntegrationFailure):
                identity({**empty, "revision": value})
        self.assertEqual(identity({**empty, "revision": 0})["revision"], 0)

    def test_identity_rejects_partial_provenance_and_extra_private_fields(self) -> None:
        good = {"state": "active", "revision": 1, "preparation_id": 5, "snapshot_id": "fnv1a64:0123456789abcdef",
                "source_route_id": "source-a", "workspace_id": "workspace-a"}
        self.assertEqual(identity(good), good)
        for bad in ({**good, "workspace_id": None}, {**good, "artifact_key": "private"},
                    {**good, "snapshot_id": "not-a-snapshot"}):
            with self.subTest(bad=bad), self.assertRaises(IntegrationFailure):
                identity(bad)

    def test_head_rejects_wrong_project_and_publication(self) -> None:
        value = {**dict.fromkeys(IDENTITY_FIELDS), "state": "empty", "revision": 0, "project_id": "visible", "publication_id": "shared", "history_epoch": 0}
        self.assertEqual(head(value, "visible", "shared"), value)
        for project, publication in (("other", "shared"), ("visible", "different")):
            with self.subTest(project=project, publication=publication), self.assertRaises(IntegrationFailure):
                head(value, project, publication)

    def test_withdrawn_identity_requires_positive_revision_and_null_selection(self) -> None:
        value = {**dict.fromkeys(IDENTITY_FIELDS | SOURCE_METADATA_FIELDS), "state": "withdrawn", "revision": 2}
        self.assertEqual(identity(value), value)
        for bad in ({**value, "revision": 0}, {**value, "state": "empty"}, {**value, "state": "active"},
                    {**value, "state": "unknown"}, {**value, "preparation_id": 1}, {**value, "source_host_id": "local-dev"}):
            with self.subTest(bad=bad), self.assertRaises(IntegrationFailure):
                identity(bad)

    def test_withdrawal_receipt_rejects_wrong_transition_and_private_fields(self) -> None:
        value = {"schema_version": "shin.publication-withdrawal-receipt.v1", "id": 5, "project_id": "visible",
                 "publication_id": "shared", "expected_revision": 1, "revision": 2, "previous_proposal_id": 3,
                 "previous_preparation_id": 7, "withdrawn_by": "dev-admin", "created_at": "2026-09-07T12:00:00Z"}
        self.assertEqual(withdrawal_receipt(value, "visible", "shared", 1, 3, 7), value)
        changes = {"schema_version": "wrong", "id": True, "project_id": "other", "publication_id": "other",
                   "expected_revision": 0, "revision": 3, "previous_proposal_id": 4, "previous_preparation_id": 8,
                   "withdrawn_by": "dev-reader", "created_at": "today", "artifact_key": "private"}
        for key, bad in changes.items():
            with self.subTest(field=key), self.assertRaises(IntegrationFailure):
                withdrawal_receipt({**value, key: bad}, "visible", "shared", 1, 3, 7)
        for field in value:
            with self.subTest(missing=field), self.assertRaises(IntegrationFailure):
                withdrawal_receipt({key: item for key, item in value.items() if key != field}, "visible", "shared", 1, 3, 7)

    def test_private_values_and_invalid_timestamps_fail_closed(self) -> None:
        with self.assertRaises(IntegrationFailure):
            no_private_values({"nested": {"value": "/private/source"}}, ["/private/source"])
        for value in (None, "todayZ", "2026-09-07T12:00:00+01:00"):
            with self.subTest(value=value), self.assertRaises(IntegrationFailure):
                timestamp(value)
        timestamp("2026-09-07T12:00:00.123Z")

    def test_detail_requires_exact_conflict_and_candidate_identity(self) -> None:
        candidate = {"preparation_id": 7, "snapshot_id": "fnv1a64:0123456789abcdef", "source_route_id": "source-a",
                     "workspace_id": "workspace-a", "source_host_id": "local-dev", "profile_id": "generic-docs",
                     "candidate_author": "author", "state": "prepared"}
        value = {"proposal": {"id": 3, "project_id": "visible", "publication_id": "shared", "preparation_id": 7,
                              "base_revision": 0, "base_preparation_id": None, "base_withdrawal_id": None, "submitted_by": "author",
                              "created_at": "2026-09-07T12:00:00Z", "state": "conflicted"},
                 "base": {**dict.fromkeys(IDENTITY_FIELDS | SOURCE_METADATA_FIELDS), "state": "empty", "revision": 0},
                 "current": {"revision": 1, "state": "active", **{key: candidate[key] for key in
                             (IDENTITY_FIELDS | SOURCE_METADATA_FIELDS) - {"revision", "state"}}},
                 "candidate": candidate, "resolution": None}
        self.assertEqual(detail(value, "visible", "shared"), value)
        withdrawn = copy.deepcopy(value)
        withdrawn["base"] = {**dict.fromkeys(IDENTITY_FIELDS | SOURCE_METADATA_FIELDS), "state": "withdrawn", "revision": 2}
        withdrawn["current"] = copy.deepcopy(withdrawn["base"])
        withdrawn["proposal"].update(base_revision=2, base_withdrawal_id=11, state="pending")
        self.assertEqual(detail(withdrawn, "visible", "shared"), withdrawn)
        for changes in ({"base_withdrawal_id": None}, {"base_withdrawal_id": True}, {"base_withdrawal_id": 0},
                        {"base_preparation_id": 7}):
            malformed = copy.deepcopy(withdrawn)
            malformed["proposal"].update(changes)
            with self.subTest(changes=changes), self.assertRaises(IntegrationFailure):
                detail(malformed, "visible", "shared")
        bad_state = copy.deepcopy(value)
        bad_state["proposal"]["state"] = "pending"
        bad_candidate = copy.deepcopy(value)
        bad_candidate["candidate"]["preparation_id"] = 8
        bad_base = copy.deepcopy(value)
        bad_base["proposal"]["base_preparation_id"] = 7
        for bad in (bad_state, bad_candidate, bad_base):
            with self.subTest(bad=bad), self.assertRaises(IntegrationFailure):
                detail(bad, "visible", "shared")

    def test_withdrawal_cases_are_registered_not_reported_without_execution(self) -> None:
        from unittest.mock import Mock
        from publication_withdrawal_checks import PublicationWithdrawalChecks

        suite = Mock()
        checks = PublicationWithdrawalChecks(suite)
        checks.run()
        self.assertEqual([call.args[0] for call in suite.record.call_args_list], [
            "publication-withdrawal-" + name for name in (
                "lifecycle-and-historical-base", "fresh-warm-refusal-before-source", "denied-invalid-no-side-effects",
                "concurrent-exact-retries", "accept-versus-withdraw-cas", "audit-transaction-rollback",
                "retention-pins-and-private-route", "restart-and-lost-reply", "missing-active-pointer-corruption")])
        self.assertTrue(all(call.args[1].__self__ is checks for call in suite.record.call_args_list))

    def test_pre_p3e13_migration_digests_are_frozen(self) -> None:
        import hashlib
        from pathlib import Path
        from publication_withdrawal_migration_checks import MIGRATION_001_012_SHA256

        paths = sorted((Path(__file__).resolve().parents[1] / "migrations").glob("[0-9][0-9][0-9]_*.sql"))[:12]
        self.assertEqual([int(path.name[:3]) for path in paths], list(range(1, 13)))
        self.assertEqual(tuple(hashlib.sha256(path.read_bytes()).hexdigest() for path in paths), MIGRATION_001_012_SHA256)

    def test_cli_retains_inherited_probe_and_bounds_optional_load(self) -> None:
        from publication_integration_test import bounded_load_requests, parse_args
        import argparse

        args = parse_args(["--binary", "/unused/server", "--ledger-probe", ""])
        self.assertEqual(args.ledger_probe, "")
        self.assertEqual(args.load_requests, 128)
        self.assertFalse(args.load)
        args = parse_args(["--binary", "/unused/server", "--load", "--load-requests", "1"])
        self.assertTrue(args.load)
        self.assertEqual(args.load_requests, 1)
        self.assertEqual(bounded_load_requests("512"), 512)
        for value in ("0", "-1", "513", "1.5", "many"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                bounded_load_requests(value)

    def test_harness_enters_full_preparation_suite_before_own_fixture(self) -> None:
        from unittest.mock import patch
        from preparation_integration_test import PreparationIntegrationSuite
        from publication_integration_test import PublicationIntegrationSuite

        # Do not construct the suite: construction acquires the real StandGuard.
        suite = PublicationIntegrationSuite.__new__(PublicationIntegrationSuite)
        suite.publication_only = False
        suite.history_only = False
        suite.checkpoint_only = False
        with patch.object(PreparationIntegrationSuite, "run", autospec=True, side_effect=RuntimeError("stop-before-stand")) as parent:
            with self.assertRaisesRegex(RuntimeError, "stop-before-stand"):
                suite.run()
            parent.assert_called_once_with(suite)

    def test_checkpoint_fixture_restores_configuration_before_resetting_state(self) -> None:
        from unittest.mock import Mock
        from publication_integration_test import PublicationIntegrationSuite

        suite = PublicationIntegrationSuite.__new__(PublicationIntegrationSuite)
        suite.stop_server = Mock()
        suite.restore_configs = Mock(side_effect=RuntimeError("stop-before-reset"))
        suite.reset_preparation_rows = Mock()
        with self.assertRaisesRegex(RuntimeError, "stop-before-reset"):
            suite.run_checkpoint_fixture()
        suite.stop_server.assert_called_once_with(expect_clean=True)
        suite.restore_configs.assert_called_once_with()
        suite.reset_preparation_rows.assert_not_called()


if __name__ == "__main__":
    unittest.main()
