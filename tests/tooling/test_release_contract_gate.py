#!/usr/bin/env python3
"""Focused metadata-gate regressions; no native build, Git or consumer access."""

import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "tests/integration/run_release_contract_tests.sh"
MACHINE = "docs/machine/shin-context-engine.manifest.json"
POLICY = "docs/machine/shin-development-v1.json"
GENERATED = "docs/kb/generated/manifest.json"
STABLE = "7.8.9"
DEV = "8.0.0-dev.9"
RC = "8.0.0-rc.1"
CONTRACT_GENERATOR = "shin-generator-v70-fixture-contract"
NATIVE_GENERATOR = "shin-generator-v71-fixture-native"
DELIVERY_GENERATOR = "shin-generator-v72-fixture-delivery"

# These inherited fields must all bind to one exact baseline, never an allowlist
# of arbitrary stable or development versions.
RELEASE_FIELDS = [
    ("session-checkpoint-v1", "release.productVersion", "session-contract-version"),
    ("execution-receipt-v1", "release.productVersion", "execution-receipt-contract-version"),
    ("execution-evidence-v1", "release.productVersion", "execution-evidence-contract-version"),
    ("retrieval-observation-v1", "release.productVersion", "retrieval-observation-contract-version"),
    ("evidence-provenance-v1", "release.productVersion", "evidence-provenance-contract-version"),
    ("worktree-diff-v1", "release.productVersion", "worktree-diff-contract-version"),
    ("universality-boundary-v1", "release", "universality-contract-release"),
    ("adoption-planner-v1", "release", "adoption-planner-contract-release"),
    ("project-adoption-v1", "release", "project-adoption-contract-release"),
    ("portable-distribution-v1", "release", "portable-distribution-contract-release"),
    ("aggregate-validation-v1", "release", "aggregate-validation-contract-release"),
    ("profile-routing-v1", "release.productVersion", "profile-routing-contract-version"),
    ("agent-readiness-v1", "release.productVersion", "agent-readiness-contract-version"),
    ("index-lifecycle-v1", "release.productVersion", "index-lifecycle-contract-version"),
    ("trust-boundary-hardening-v1", "version", "trust-boundary-contract-version"),
    ("static-analysis-v1", "release.productVersion", "static-analysis-contract-version"),
]
OTHER_CONTRACTS = [
    "retrieval-evidence-v1", "agent-context-pack-v1", "agent-context-pack-diff-v1",
    "batch-retrieval-v1", "retrieval-diagnose-v1",
    "snapshot-diff-v1",
]


class ReleaseContractGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-release-gate-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "report.json"
        self.jq = shutil.which("jq")
        self.assertIsNotNone(self.jq, "jq is required; do not skip the gate tests")
        paths = [MACHINE, POLICY, "README.md",
                 "integrations/codex/skills/shin-context/SKILL.md",
                 "integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md"]
        paths += [f"docs/machine/{name}.json" for name, _, _ in RELEASE_FIELDS]
        paths += [f"docs/machine/{name}.json" for name in OTHER_CONTRACTS]
        bundles = json.loads((ROOT / "docs/machine/universality-boundary-v1.json").read_text())["integrationBundles"]
        self.bundle = bundles[0]
        paths += bundles
        for relative in paths:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        (self.root / GENERATED).parent.mkdir(parents=True, exist_ok=True)
        self.write(GENERATED, {"generator_version": NATIVE_GENERATOR})
        for name, field, _ in RELEASE_FIELDS:
            self.change(f"docs/machine/{name}.json", field, STABLE)

        self.write("version.json", {
            "schema_version": "shin.version.v1", "version": DEV,
            "stability": "development", "generator_version": NATIVE_GENERATOR,
            "generated_json_decoder": {
                "library": "simdjson", "version": "4.6.10", "api": "dom",
                "scope": "generated-jsonl-only",
            },
        })
        self.write("help.json", {"commands": [
            {"name": name} for name in self.read(MACHINE)["commandCatalog"]
        ]})
        self.change(POLICY, "stableLane.version", STABLE)
        self.change(POLICY, "developmentLane.contractBaselineVersion", STABLE)
        self.change(POLICY, "stableLane.generator", DELIVERY_GENERATOR)
        self.change(POLICY, "developmentLane.coreGenerator", NATIVE_GENERATOR)
        self.change(POLICY, "developmentLane.contractBaselineGenerator", CONTRACT_GENERATOR)
        self.change(MACHINE, "project.stableBaseline.generator", CONTRACT_GENERATOR)
        self.change("docs/machine/evidence-provenance-v1.json", "release.generator", CONTRACT_GENERATOR)
        self.change("docs/machine/trust-boundary-hardening-v1.json", "generator", CONTRACT_GENERATOR)
        self.change(MACHINE, "project.stableBaseline.version", STABLE)
        self.change(MACHINE, "project.stableBaseline.tag", "v" + STABLE)
        self.set_lane(DEV)
        self.tool = self.root / "fixture-tool"
        self.tool.write_text(
            '#!/usr/bin/env bash\nset -euo pipefail\n'
            'root="$(dirname "$0")"\n'
            'case "$*" in\n'
            '  --version) cat "$root/version.json" ;;\n'
            '  help) cat "$root/help.json" ;;\n'
            '  *) exit 64 ;;\nesac\n', encoding="utf-8")
        self.tool.chmod(0o755)

    def read(self, relative):
        return json.loads((self.root / relative).read_text(encoding="utf-8"))

    def test_universality_uses_contract_baseline_not_current_delivery(self):
        boundary = self.read("docs/machine/universality-boundary-v1.json")
        for relative in boundary["integrationBundles"]:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        shutil.copytree(ROOT / "profiles", self.root / "profiles")
        (self.root / "src").mkdir()
        (self.root / "Makefile").write_text("\n".join(
            f".PHONY: {target}\n{target}:\n\t@echo fixture-release-plan\n"
            for target in boundary["core"]["releaseTargets"]))
        self.change(POLICY, "stableLane.version", "7.9.1")
        gate = ROOT / "tests/integration/run_universality_tests.sh"
        report = self.root / "universality.json"
        process = subprocess.run(["bash", str(gate), self.jq, str(self.root), str(report)],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(process.returncode, 0, process.stderr or process.stdout)
        self.assertEqual(process.stderr, "")
        result = json.loads(report.read_text())
        self.assertTrue(result["ok"])
        self.assertEqual(result["contract_release"], STABLE)
        self.assertEqual(result["version_policy"], "candidate-contract-baseline")

        self.change(POLICY, "developmentLane.contractBaselineVersion", "7.9.1")
        process = subprocess.run(["bash", str(gate), self.jq, str(self.root), str(report)],
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(process.returncode, 1, process.stderr or process.stdout)
        failed = {item["id"] for item in json.loads(report.read_text())["checks"] if not item["ok"]}
        self.assertIn("contract-release", failed)
        self.assertIn("project-adoption-contract-release", failed)

    def test_universality_refuses_missing_or_invalid_boundary_lists(self):
        relative = "docs/machine/universality-boundary-v1.json"
        original = self.read(relative)
        for field in ("forbiddenConsumerIdentifiers", "forbiddenReleasePlanTokens", "releaseTargets"):
            for value in (None, [], "invalid", [""], [1]):
                with self.subTest(field=field, value=value):
                    self.write(relative, original)
                    self.change(relative, "core." + field, value)
                    report = self.root / "universality-invalid.json"
                    process = subprocess.run([
                        "bash", str(ROOT / "tests/integration/run_universality_tests.sh"),
                        self.jq, str(self.root), str(report)],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(process.returncode, 2)
                    self.assertIn("boundary lists must be explicit", process.stderr)
                    self.assertFalse(report.exists())

    def write(self, relative, value):
        (self.root / relative).write_text(json.dumps(value) + "\n", encoding="utf-8")

    def change(self, relative, field, value):
        document = self.read(relative)
        node = document
        keys = field.split(".")
        for key in keys[:-1]:
            node = node[key]
        node[keys[-1]] = value
        self.write(relative, document)

    def set_lane(self, version):
        development = "-dev." in version or "-rc." in version
        (self.root / "VERSION").write_text(version + "\n", encoding="utf-8")
        self.change("version.json", "version", version)
        self.change("version.json", "stability", "development" if development else "stable")
        generator = NATIVE_GENERATOR if development else CONTRACT_GENERATOR
        self.change("version.json", "generator_version", generator)
        self.change(GENERATED, "generator_version", generator)
        self.change(MACHINE, "project.version", version)
        self.change(MACHINE, "project.status", "development" if development else "stable")
        self.change(MACHINE, "project.release.tag", None if development else "v" + version)
        self.change(MACHINE, "project.release.date", None if development else "2026-01-01")
        self.change(POLICY, "developmentLane.coreVersion", version)

    def run_gate(self, *, failing_check=None, setup_error=None):
        self.report.unlink(missing_ok=True)
        result = subprocess.run(
            ["bash", str(GATE), self.jq, str(self.tool), str(self.root), str(self.report)],
            text=True, capture_output=True, check=False, timeout=20,
        )
        if setup_error:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(setup_error, result.stderr)
            self.assertFalse(self.report.exists())
            return None
        self.assertTrue(self.report.exists(), result.stderr)
        report = self.read("report.json")
        failures = {check["id"] for check in report["checks"] if not check["ok"]}
        if failing_check:
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(report["ok"])
            self.assertIn(failing_check, failures)
            self.assertFalse(report["stable_release_metadata_validated"])
        else:
            self.assertEqual(result.returncode, 0, f"{result.stderr}\n{failures}")
            self.assertTrue(report["ok"])
            self.assertEqual(failures, set())
        return report

    def replace_skill_minimum(self, relative, old, new):
        path = self.root / relative
        contents = path.read_text(encoding="utf-8")
        marker = f"Shin v{old} or newer"
        self.assertIn(marker, contents)
        path.write_text(contents.replace(marker, f"Shin v{new} or newer", 1), encoding="utf-8")

    def test_integration_skill_minimums_are_route_specific_and_monotonic(self):
        codex = "integrations/codex/skills/shin-context/SKILL.md"
        hermes = "integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md"
        self.run_gate()

        self.replace_skill_minimum(codex, "2.0.0", "2.1.0")
        self.replace_skill_minimum(hermes, "2.0.0", "2.0.1")
        self.run_gate()

        for relative, original, regression in (
            (codex, "2.0.0", "1.27.4"),
            (hermes, "2.0.0", "1.27.4"),
        ):
            with self.subTest(skill=relative):
                text = (self.root / relative).read_text(encoding="utf-8")
                self.replace_skill_minimum(relative,
                                           "2.1.0" if relative == codex else "2.0.1",
                                           regression)
                self.run_gate(setup_error="integration skills must retain approved per-client minimums")
                (self.root / relative).write_text(text, encoding="utf-8")

    def test_integration_skill_generator_and_managed_guidance_remain_required(self):
        codex = "integrations/codex/skills/shin-context/SKILL.md"
        hermes = "integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md"
        contents = (self.root / codex).read_text(encoding="utf-8")
        (self.root / codex).write_text(contents.replace("## Normal Work", "## Removed Workflow"),
                                        encoding="utf-8")
        self.run_gate(setup_error="integration skills must retain approved per-client minimums")

        (self.root / codex).write_text(contents, encoding="utf-8")
        for relative in (codex, hermes):
            text = (self.root / relative).read_text(encoding="utf-8")
            (self.root / relative).write_text(
                text.replace("shin-generator-v24-objective-cpp", "shin-generator-v22-compact-lexical"),
                encoding="utf-8",
            )
            self.run_gate(setup_error="integration skills must retain approved per-client minimums")
            (self.root / relative).write_text(text, encoding="utf-8")

    def test_development_revisions_are_exact_and_not_stable_releases(self):
        for version in (DEV, "8.0.0-dev.10", RC):
            with self.subTest(version=version):
                self.set_lane(version)
                report = self.run_gate()
                self.assertEqual(report["version"], version)
                self.assertEqual(report["stability"], "development")
                self.assertEqual(report["contract_version"], STABLE)
                self.assertEqual(report["version_policy"], "candidate-stable-baseline")
                self.assertFalse(report["stable_release_metadata_validated"])

    def test_stable_lane_needs_no_development_policy(self):
        self.set_lane(STABLE)
        (self.root / POLICY).unlink()
        report = self.run_gate()
        self.assertEqual(report["version_policy"], "stable")
        self.assertEqual(report["contract_version"], STABLE)
        self.assertTrue(report["stable_release_metadata_validated"])

    def test_delivery_updates_do_not_rebind_inherited_contracts(self):
        self.change(POLICY, "stableLane.version", "7.9.1")
        report = self.run_gate()
        self.assertEqual(report["contract_version"], STABLE)
        policy = self.read(POLICY)
        del policy["developmentLane"]["contractBaselineVersion"]
        self.write(POLICY, policy)
        self.run_gate(failing_check="development-baseline-canonical")

    def test_rc_universality_retains_pinned_contract_baseline(self):
        self.set_lane(RC)
        self.test_universality_uses_contract_baseline_not_current_delivery()

    def test_rc_requires_exact_identity_and_explicit_policy(self):
        self.set_lane(RC)
        self.change(POLICY, "developmentLane.coreVersion", DEV)
        self.run_gate(failing_check="development-policy-core-version")
        self.change(POLICY, "developmentLane.coreVersion", RC)
        self.change(MACHINE, "project.release.tag", "v8.0.0")
        self.run_gate(failing_check="machine-release-tag")
        self.change(MACHINE, "project.release.tag", None)
        self.change(MACHINE, "project.release.date", "2026-10-02")
        self.run_gate(failing_check="development-release-date")
        (self.root / POLICY).unlink()
        self.run_gate(setup_error="Could not open")

    def test_identity_drift_and_false_release_claims_fail(self):
        cases = [
            ("version.json", "version", "8.0.0-dev.10", "version-result-value"),
            ("version.json", "stability", "stable", "version-result-stability"),
            (MACHINE, "project.version", "8.0.0-dev.10", "machine-project-version"),
            (MACHINE, "project.release.tag", "v8.0.0", "machine-release-tag"),
            (MACHINE, "project.release.date", "2026-01-01", "development-release-date"),
            (MACHINE, "project.status", "stable", "development-project-status"),
            (MACHINE, "project.stableBaseline.version", "7.8.8", "development-baseline-version"),
            (MACHINE, "project.stableBaseline.tag", "v7.8.8", "development-baseline-tag"),
            (MACHINE, "project.stableBaseline.policy", "unreviewed.json", "development-baseline-policy"),
            (POLICY, "developmentLane.coreVersion", "8.0.0-dev.10", "development-policy-core-version"),
            (POLICY, "schemaVersion", "unreviewed.v1", "development-policy-schema"),
            (POLICY, "status", "draft", "development-policy-status"),
            (POLICY, "cutover.automatic", True, "development-policy-no-automatic-cutover"),
            (POLICY, "developmentLane.separateCheckoutAndBuilds", False, "development-policy-separate-builds"),
            (POLICY, "stableLane.generator", "different", "development-delivery-generator-canonical"),
            (POLICY, "stableLane.version", None, "development-delivery-canonical"),
        ]
        for relative, field, value, check in cases:
            with self.subTest(check=check):
                original = self.read(relative)
                self.change(relative, field, value)
                self.run_gate(failing_check=check)
                self.write(relative, original)

    def test_baseline_must_be_canonical_and_precede_candidate(self):
        for version in ("8.0.0", "9.0.0", "7.8.9-dev.1", "07.8.9", None):
            with self.subTest(version=version):
                self.change(POLICY, "developmentLane.contractBaselineVersion", version)
                self.run_gate(failing_check="development-version-after-baseline")

    def test_baseline_order_uses_numeric_components_without_overflow(self):
        for baseline, candidate in (
            ("7.8.9", "7.8.10-dev.0"),
            ("7.9.9", "7.10.0-dev.0"),
            ("9.0.0", "10.0.0-dev.0"),
            ("9007199254740992.0.0", "9007199254740993.0.0-dev.0"),
        ):
            with self.subTest(baseline=baseline, candidate=candidate):
                self.set_lane(candidate)
                self.change(POLICY, "stableLane.version", baseline)
                self.change(POLICY, "developmentLane.contractBaselineVersion", baseline)
                self.change(MACHINE, "project.stableBaseline.version", baseline)
                self.change(MACHINE, "project.stableBaseline.tag", "v" + baseline)
                for name, field, _ in RELEASE_FIELDS:
                    self.change(f"docs/machine/{name}.json", field, baseline)
                self.run_gate()

    def test_every_inherited_version_remains_exact_in_both_lanes(self):
        for lane in (STABLE, DEV, RC):
            self.set_lane(lane)
            for name, field, check in RELEASE_FIELDS:
                for mismatch in ("7.8.8", DEV, None):
                    with self.subTest(lane=lane, contract=name, mismatch=mismatch):
                        relative = f"docs/machine/{name}.json"
                        self.change(relative, field, mismatch)
                        self.run_gate(failing_check=check)
                        self.change(relative, field, STABLE)

    def test_stable_tag_and_stability_remain_strict(self):
        self.set_lane(STABLE)
        self.change(MACHINE, "project.release.tag", None)
        self.run_gate(failing_check="machine-release-tag")
        self.change(MACHINE, "project.release.tag", "v" + STABLE)
        self.change("version.json", "stability", "development")
        self.run_gate(failing_check="version-result-stability")

    def test_safety_generator_and_catalog_checks_remain_in_both_lanes(self):
        cases = [
            (GENERATED, "generator_version", "different", "generated-generator-version"),
            ("version.json", "schema_version", "wrong.v1", "version-result-schema"),
            ("help.json", "commands", [], "command-catalog"),
            (MACHINE, "commands.inspect", None, "inspect-command-documented"),
            (MACHINE, "commands.read", None, "read-command-documented"),
            ("docs/machine/agent-readiness-v1.json", "command.consumerMutationAllowed", True,
             "agent-readiness-no-build-or-mutation"),
            ("docs/machine/index-lifecycle-v1.json", "commands.prune.dryRunDefault", False,
             "index-lifecycle-dry-run-default"),
            ("docs/machine/static-analysis-v1.json", "policy.warningsAsErrors", False,
             "static-analysis-warnings-as-errors"),
            (self.bundle,
             "coreReleaseDependency", True, "integration-bundles-not-release-dependencies"),
        ]
        check_ids = []
        for lane in (STABLE, DEV, RC):
            self.set_lane(lane)
            check_ids.append({check["id"] for check in self.run_gate()["checks"]})
            for relative, field, value, check in cases:
                with self.subTest(lane=lane, check=check):
                    original = self.read(relative)
                    self.change(relative, field, value)
                    self.run_gate(failing_check=check)
                    self.write(relative, original)
        self.assertTrue(check_ids[0] < check_ids[1], "development must retain every stable check")
        self.assertEqual(check_ids[1], check_ids[2], "RC must retain every development check")

    def test_three_generator_identities_are_independent(self):
        report = self.run_gate()
        self.assertEqual(report["runtime_generator"], NATIVE_GENERATOR)
        self.assertEqual(report["contract_generator"], CONTRACT_GENERATOR)
        self.assertEqual(report["stable_delivery"], {
            "version": STABLE, "generator": DELIVERY_GENERATOR,
            "verification": "declared-metadata-not-live-installation"})
        self.change(POLICY, "stableLane.version", "7.9.1")
        self.change(POLICY, "stableLane.generator", "shin-generator-v73-later-delivery")
        report = self.run_gate()
        self.assertEqual(report["runtime_generator"], NATIVE_GENERATOR)
        self.assertEqual(report["contract_generator"], CONTRACT_GENERATOR)
        self.assertFalse(report["stable_release_metadata_validated"])

    def test_current_generator_requires_an_explicit_pin_and_matching_snapshot(self):
        for value in (None, "", CONTRACT_GENERATOR, DELIVERY_GENERATOR, "arbitrary"):
            with self.subTest(value=value):
                self.change(POLICY, "developmentLane.coreGenerator", value)
                self.run_gate(failing_check="development-policy-core-generator")
        self.change(POLICY, "developmentLane.coreGenerator", NATIVE_GENERATOR)
        for value in (CONTRACT_GENERATOR, DELIVERY_GENERATOR):
            self.change(GENERATED, "generator_version", value)
            self.run_gate(failing_check="generated-generator-version")
        self.change(GENERATED, "generator_version", NATIVE_GENERATOR)
        self.change("version.json", "generator_version", "arbitrary")
        self.change(GENERATED, "generator_version", "arbitrary")
        self.change(POLICY, "developmentLane.coreGenerator", "arbitrary")
        self.run_gate(failing_check="version-generator-canonical")

    def test_inherited_generator_cannot_follow_current_or_delivery_implicitly(self):
        for value in (None, "", NATIVE_GENERATOR, DELIVERY_GENERATOR):
            with self.subTest(value=value):
                self.change(POLICY, "developmentLane.contractBaselineGenerator", value)
                self.run_gate(failing_check="evidence-provenance-contract-generator")
        self.change(POLICY, "developmentLane.contractBaselineGenerator", CONTRACT_GENERATOR)
        self.change(MACHINE, "project.stableBaseline.generator", NATIVE_GENERATOR)
        self.run_gate(failing_check="development-baseline-generator")
        self.change(MACHINE, "project.stableBaseline.generator", CONTRACT_GENERATOR)
        for relative, field, check in (
            ("docs/machine/evidence-provenance-v1.json", "release.generator", "evidence-provenance-contract-generator"),
            ("docs/machine/trust-boundary-hardening-v1.json", "generator", "trust-boundary-contract-generator"),
        ):
            self.change(relative, field, NATIVE_GENERATOR)
            self.run_gate(failing_check=check)
            self.change(relative, field, CONTRACT_GENERATOR)

    def test_stable_release_retains_same_generator_requirement(self):
        self.set_lane(STABLE)
        (self.root / POLICY).unlink()
        self.change("version.json", "generator_version", NATIVE_GENERATOR)
        self.change(GENERATED, "generator_version", NATIVE_GENERATOR)
        report = self.run_gate(failing_check="evidence-provenance-contract-generator")
        self.assertIsNone(report["stable_delivery"])
        self.assertIn("trust-boundary-contract-generator", {
            check["id"] for check in report["checks"] if not check["ok"]})

    def test_missing_and_malformed_generator_pins_fail_closed(self):
        for field, check in (("coreGenerator", "development-policy-core-generator"),
                             ("contractBaselineGenerator", "development-baseline-generator-canonical")):
            original = self.read(POLICY)
            document = self.read(POLICY)
            del document["developmentLane"][field]
            self.write(POLICY, document)
            self.run_gate(failing_check=check)
            self.write(POLICY, original)
        for value in (None, "", "shin-generator-v01-bad", "anything", DELIVERY_GENERATOR + "\n", [], {}):
            self.change(POLICY, "stableLane.generator", value)
            self.run_gate(failing_check="development-delivery-generator-canonical")
        self.change(POLICY, "stableLane.generator", DELIVERY_GENERATOR)
        self.change(POLICY, "stableLane.version", STABLE + "\n")
        self.run_gate(failing_check="development-delivery-canonical")

    def test_missing_or_malformed_policy_fails_closed(self):
        (self.root / POLICY).unlink()
        self.run_gate(setup_error="Could not open")
        (self.root / POLICY).write_text("{invalid", encoding="utf-8")
        self.run_gate(setup_error="Bad JSON")
        self.write(POLICY, {})
        self.run_gate(failing_check="development-policy-schema")

    def test_missing_explicit_null_release_fields_fail(self):
        document = self.read(MACHINE)
        del document["project"]["release"]["tag"]
        self.write(MACHINE, document)
        self.run_gate(failing_check="development-release-fields-present")

    def test_multiple_policy_documents_fail_closed(self):
        path = self.root / POLICY
        contents = path.read_text(encoding="utf-8")
        path.write_text(contents + contents, encoding="utf-8")
        self.run_gate(failing_check="development-policy-single-document")

    def test_unsupported_or_multiline_versions_fail_before_validation(self):
        for version in ("8.0.0-rc.01", "8.0.0-rc", "8.0.0-beta.1", "8.0.0-dev.01", "v8.0.0", "8.0.\n0-dev.9", ""):
            with self.subTest(version=version):
                (self.root / "VERSION").write_text(version + "\n", encoding="utf-8")
                self.run_gate(setup_error="VERSION must be a canonical")


class SourceRebuildCutoverTests(unittest.TestCase):
    def test_previous_generator_is_retired_when_the_current_identity_changes(self):
        types = (ROOT / 'src/context_types.h').read_text()
        current = re.search(r'\bkGeneratorVersion\s*=\s*"([^"]+)"', types).group(1)
        policy = (ROOT / 'observer/src/local_index_rebuild.h').read_text()
        retired = re.findall(r'"(shin-generator-v[1-9][0-9]*-[a-z0-9-]+)"', policy)
        ordinal = lambda value: int(re.fullmatch(r'shin-generator-v([1-9][0-9]*)-[a-z0-9-]+', value).group(1))
        self.assertTrue(retired, 'Source rebuild policy must identify the retired predecessor')
        self.assertNotIn(current, retired, 'A current format is not a retired snapshot')
        self.assertTrue(all(ordinal(value) < ordinal(current) for value in retired))
        self.assertEqual(max(map(ordinal, retired)) + 1, ordinal(current),
                         'Retire the previous generator and qualify its source rebuild before this cutover')
        previous_kit = json.loads((ROOT / '.github/fixtures/macos-previous-generator.json').read_text())
        self.assertIn(previous_kit['generator'], retired)
        self.assertEqual(ordinal(previous_kit['generator']) + 1, ordinal(current),
                         'Exact package qualification must exercise the previous generator, not only a same-format update')


if __name__ == "__main__":
    unittest.main()
