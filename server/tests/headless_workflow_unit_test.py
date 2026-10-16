#!/usr/bin/env python3
"""Fail-closed installed-delivery selection without starting the test stand."""

from pathlib import Path
from types import SimpleNamespace
import json
import tempfile
import unittest

from headless_workflow_test import DELIVERY, IntegrationFailure, delivery_inputs, run_binary_rollback
from integration_test import IntegrationSuite, SERVER_VERSION
from headless_agent_probe import verify_checkpoint_storage


class InstalledDeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prefix = self.root / "prefix"
        self.args = SimpleNamespace(installed_prefix=self.prefix)
        for name, tool in (("core", "shin-context"), ("server", "shin-server"),
                           ("observer", "shin-observer")):
            path = self.prefix / "bin" / tool
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
            setattr(self.args, name, path)
        self.files = [self.prefix / "share/shin-candidate/agent" / name
                      for name in ("AGENT-WORKFLOW.md", "workflow-examples.json")]
        self.files.append(self.prefix / "share/shin-context/profiles/generic-docs.json")
        for path in self.files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")

    def test_installed_inputs_selected(self):
        delivery, profile = delivery_inputs(self.args)
        self.assertEqual(delivery, self.files[0].parent)
        self.assertEqual(profile, self.files[2])

    def test_agent_checkpoint_must_be_private_and_independent(self):
        path = self.root / "checkpoint.json"
        path.write_text("{}")
        path.chmod(0o600)
        verify_checkpoint_storage(path)
        path.chmod(0o644)
        with self.assertRaises(IntegrationFailure):
            verify_checkpoint_storage(path)

    def test_agent_checkpoint_rejects_temporary_hardlink_alias(self):
        temporary = self.root / "checkpoint.tmp"
        temporary.write_text("{}")
        temporary.chmod(0o600)
        published = self.root / "checkpoint.json"
        published.hardlink_to(temporary)
        with self.assertRaises(IntegrationFailure):
            verify_checkpoint_storage(published)
        temporary.unlink()
        verify_checkpoint_storage(published)

    def test_agent_checkpoint_rejects_symlink(self):
        target = self.root / "checkpoint.json"
        target.write_text("{}")
        target.chmod(0o600)
        alias = self.root / "checkpoint-link.json"
        alias.symlink_to(target)
        with self.assertRaises(IntegrationFailure):
            verify_checkpoint_storage(alias)

    def test_source_mode_unchanged(self):
        self.args.installed_prefix = None
        self.assertEqual(delivery_inputs(self.args)[0], DELIVERY)

    def test_missing_inputs_never_fall_back(self):
        for path in self.files:
            with self.subTest(path=path.name):
                path.unlink()
                with self.assertRaises(IntegrationFailure):
                    delivery_inputs(self.args)
                path.write_text("fixture")

    def test_external_binary_rejected(self):
        self.args.core = self.root / "other-core"
        self.args.core.write_text("fixture")
        with self.assertRaises(IntegrationFailure):
            delivery_inputs(self.args)

    def test_external_input_symlink_rejected(self):
        outside = self.root / "outside"
        outside.write_text("fixture")
        self.files[0].unlink()
        self.files[0].symlink_to(outside)
        with self.assertRaises(IntegrationFailure):
            delivery_inputs(self.args)

    def test_binary_symlink_rejected(self):
        outside = self.root / "outside"
        outside.write_text("fixture")
        self.args.core.unlink()
        self.args.core.symlink_to(outside)
        with self.assertRaises(IntegrationFailure):
            delivery_inputs(self.args)

    def test_version_admission_is_exact_for_each_selected_binary(self):
        envelope = {"schema_version": "shin.server-response.v1", "tool": "shin-server",
                    "version": SERVER_VERSION, "stability": "development", "ok": True, "result": {}}
        suite = SimpleNamespace(expected_server_version=SERVER_VERSION)
        IntegrationSuite.assert_envelope(suite, envelope)
        suite.expected_server_version = "2.0.0-dev.37"
        with self.assertRaises(IntegrationFailure):
            IntegrationSuite.assert_envelope(suite, envelope)
        envelope["version"] = suite.expected_server_version
        IntegrationSuite.assert_envelope(suite, envelope)
        suite.expected_server_version = SERVER_VERSION
        with self.assertRaises(IntegrationFailure):
            IntegrationSuite.assert_envelope(suite, envelope)

    def test_rollback_rejects_incompatible_control_before_mutation(self):
        previous = self.root / "previous"
        previous.write_text("different binary")
        def invoke(command, _):
            contracts = {"configuration_schema": "old" if command[0] == str(previous) else "current"}
            value = {"ok": True, "tool": "shin-server", "version": "fixture",
                     "result": {"core_version": "same", "contracts": contracts}}
            return SimpleNamespace(stdout=json.dumps(value))
        suite = SimpleNamespace(binary=self.args.server, expected_server_version=SERVER_VERSION,
                                run_process=invoke)
        with self.assertRaisesRegex(IntegrationFailure, "matching protocol"):
            run_binary_rollback(suite, None, previous)

    def test_rollback_rejects_identical_control_before_mutation(self):
        value = {"ok": True, "tool": "shin-server", "version": "fixture",
                 "result": {"core_version": "same", "contracts": {}}}
        suite = SimpleNamespace(binary=self.args.server, expected_server_version=SERVER_VERSION,
                                run_process=lambda *_: SimpleNamespace(stdout=json.dumps(value)))
        with self.assertRaisesRegex(IntegrationFailure, "differ from the candidate"):
            run_binary_rollback(suite, None, self.args.server)

    def test_rollback_rejects_different_core_with_matching_protocol_before_mutation(self):
        previous = self.root / "previous-core"
        previous.write_text("different binary")

        def invoke(command, _):
            core = "previous" if command[0] == str(previous) else "current"
            value = {"ok": True, "tool": "shin-server", "version": "fixture",
                     "result": {"core_version": core, "contracts": {"configuration_schema": "same"}}}
            return SimpleNamespace(stdout=json.dumps(value))

        suite = SimpleNamespace(binary=self.args.server, expected_server_version=SERVER_VERSION,
                                run_process=invoke)
        with self.assertRaisesRegex(IntegrationFailure, "matching protocol and Core identities"):
            run_binary_rollback(suite, None, previous)


if __name__ == "__main__":
    unittest.main()
