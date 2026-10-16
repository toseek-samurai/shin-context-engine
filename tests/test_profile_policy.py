#!/usr/bin/env python3
"""Read-only policy inspection and pending generic-source default regressions."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(sys.argv.pop(1)).resolve()
REGISTRY = Path(sys.argv.pop(1)).resolve()


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="shin-policy-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "root"
        self.root.mkdir()
        self.registry = self.base / "profiles"
        self.registry.mkdir()
        (self.root / "README.md").write_text("# Policy fixture\n")
        self.profile = {"id": "fixture", "sourceLabel": "Fixture",
                        "rootFiles": ["README.md"], "bootstrapOnly": True}

    def invoke(self, *args, expected=0, generic=False):
        result = subprocess.run(
            [str(TOOL), "--profile", "generic-source" if generic else "fixture",
             "--profile-dir", str(REGISTRY if generic else self.registry),
             "--root", str(self.root), "--out-dir", str(self.base / "index"), *args],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if expected == 2:
            self.assertFalse(result.stdout, result.stdout)
            value = json.loads(result.stderr)
            self.assertEqual(value["schema_version"], "shin.error.v1")
            return value
        self.assertFalse(result.stderr, result.stderr)
        return json.loads(result.stdout)

    def save(self):
        (self.registry / "fixture.json").write_text(json.dumps(self.profile))

    def policy(self, *args, expected=0):
        self.save()
        return self.invoke("profile-policy", *args, expected=expected)

    def test_defaults_aliases_and_no_mutation(self):
        self.profile["markdown_chunk_chars"] = 1300
        self.save()
        before = {p: p.read_bytes() for p in self.base.rglob("*") if p.is_file()}
        value = self.invoke("profile-policy")
        self.assertEqual(value["fields"]["markdownChunkChars"],
                         {"value": 1300, "origin": "profile", "input_key": "markdown_chunk_chars"})
        self.assertEqual(value["fields"]["agentStartPaths"]["origin"], "derived-root-files")
        self.assertEqual(value["fields"]["sourceFactExtractors"]["origin"], "core-default")
        self.assertFalse(value["readiness_assessed"])
        self.assertFalse(value["capabilities"]["activation"])
        self.assertEqual(before, {p: p.read_bytes() for p in self.base.rglob("*") if p.is_file()})
        self.assertFalse((self.base / "index").exists())

    def test_empty_input_fallback_is_explicit(self):
        self.profile.update(sourceExtensions=[], agentStartPaths=[])
        fields = self.policy()["fields"]
        self.assertEqual(fields["sourceExtensions"]["value"], [".md", ".json"])
        self.assertEqual(fields["sourceExtensions"]["origin"], "core-default-empty-input")
        self.assertEqual(fields["agentStartPaths"]["value"], ["README.md"])

    def test_conflicting_aliases(self):
        self.profile.update(sourceFactMaxFiles=100, source_fact_max_files=200)
        value = self.policy(expected=1)
        self.assertFalse(value["ok"])
        self.assertIn("conflicting-field-aliases", [i["code"] for i in value["issues"]])

    def test_invalid_types_and_overflow(self):
        for key, value in [("sourceFactMaxFiles", "100"), ("sourceFactMaxFiles", 2**40),
                           ("rootFiles", ["README.md", 7]), ("jsonKeySections", "false")]:
            with self.subTest(key=key, value=value):
                old = dict(self.profile)
                self.profile[key] = value
                result = self.policy(expected=1)
                self.assertIn("invalid-field-type", [i["code"] for i in result["issues"]])
                self.profile = old

    def test_unknown_layers_and_fields_rejected(self):
        for key in ["organizationConstraints", "taskOverlay", "extends", "allowExport", "sourceFactMaxFile"]:
            with self.subTest(key=key):
                self.profile[key] = {"token": "do-not-copy-unknown-value"}
                value = self.policy(expected=1)
                self.assertIn("unsupported-profile-field", [i["code"] for i in value["issues"]])
                self.assertNotIn("do-not-copy-unknown-value", json.dumps(value))
                del self.profile[key]

    def test_malformed_rule_sets_not_silently_dropped(self):
        for key, value in [("topicRules", {}), ("aliases", ["not-a-rule"]),
                           ("evalCases", "not-an-array"), ("packEvalCases", [False])]:
            with self.subTest(key=key):
                self.profile[key] = value
                result = self.policy(expected=1)
                self.assertIn("invalid-rule-set-type", [i["code"] for i in result["issues"]])
                del self.profile[key]

    def test_duplicate_json_keys_rejected(self):
        self.save()
        path = self.registry / "fixture.json"
        path.write_text(path.read_text()[:-1] + ', "bootstrapOnly": false}')
        value = self.invoke("profile-policy", expected=2)
        self.assertFalse(value["ok"])
        self.assertIn("ambiguous-profile-policy", json.dumps(value))

    def test_revision_conflict(self):
        value = self.policy()
        revision = value["revision"]["profile_config_hash"]
        self.assertEqual(self.invoke("profile-policy", "--expect-profile-hash", revision), value)
        self.profile["summary"] = "changed"
        self.save()
        result = self.invoke("profile-policy", "--expect-profile-hash", revision, expected=2)
        self.assertIn("profile-policy-revision-conflict", json.dumps(result))

    def test_cli_rejects_write_and_override_options(self):
        self.save()
        for args in [("--apply",), ("--task-overlay", "anything"), ("--expect-profile-hash",),
                     ("--expect-profile-hash", "x", "--expect-profile-hash", "y"), ("--source-scan",)]:
            with self.subTest(args=args):
                self.assertFalse(self.invoke("profile-policy", *args, expected=2)["ok"])

    def test_profile_validation_not_waived(self):
        self.profile["rootFiles"] = ["../outside.md"]
        result = self.policy(expected=1)
        self.assertGreater(result["profile_validation"]["errors"], 0)

    def test_policy_read_does_not_call_stale_index_ready(self):
        self.save()
        self.invoke("build")
        (self.root / "README.md").write_text("# Modified but uncommitted\n")
        value = self.invoke("profile-policy")
        self.assertTrue(value["ok"])
        self.assertFalse(value["readiness_assessed"])
        self.assertEqual(value["index_state"], "not-assessed")
        self.assertFalse(self.invoke("check", expected=1)["ok"])

    def test_profile_revision_change_preserves_index_staleness(self):
        self.save()
        self.invoke("build")
        old_hash = self.invoke("profile-policy")["revision"]["profile_config_hash"]
        self.profile["markdownChunkChars"] = 1300
        self.save()
        new_hash = self.invoke("profile-policy")["revision"]["profile_config_hash"]
        self.assertNotEqual(old_hash, new_hash)
        self.assertFalse(self.invoke("check", expected=1)["ok"])

    def test_generic_monorepo_no_git_and_generated_exclusion(self):
        files = {"packages/ui/src/policy.ts": "export function activePolicy() { return 1; }\n",
                 "packages/ui/storybook-static/preview.js": "function generatedPreview() {}\n",
                 ".nuget/cache/dependency.cs": "class CachedDependency {}\n",
                 "node_modules/ignored.js": "function thirdParty() {}\n"}
        for name, content in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        discovery = self.invoke("discover", generic=True)
        self.assertFalse(discovery["has_git"])
        self.assertEqual(discovery["counts"]["ts_files"], 1)
        self.invoke("build", generic=True)
        facts = [json.loads(line) for line in
                 (self.base / "index/docs/kb/generated/facts.jsonl").read_text().splitlines()]
        paths = {item.get("path") for item in facts}
        self.assertIn("packages/ui/src/policy.ts", paths)
        for path in files.keys() - {"packages/ui/src/policy.ts"}:
            self.assertNotIn(path, paths)

    def test_generic_fallback_formats_are_admitted(self):
        samples = {"worker.py": "def calculate_total():\n    return 1\n",
                   "worker.go": "package worker\nfunc CalculateTotal() int { return 1 }\n",
                   "worker.rs": "pub fn calculate_total() -> i32 { 1 }\n",
                   "query.sql": "SELECT total FROM payments;\n",
                   "settings.yaml": "service: fixture\n",
                   "settings.json": '{"service":"fixture"}\n',
                   "app.rsx": '<App><RESTQuery id="LoadItems" query="/items" /></App>\n'}
        for name, content in samples.items():
            (self.root / name).write_text(content)
        self.invoke("profile-policy", generic=True)
        self.invoke("build", generic=True)
        manifest = json.loads((self.base / "index/docs/kb/generated/manifest.json").read_text())
        serialized = json.dumps(manifest)
        for name in samples:
            self.assertIn(name, serialized)


if __name__ == "__main__":
    unittest.main()
