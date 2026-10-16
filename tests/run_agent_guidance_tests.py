#!/usr/bin/env python3
"""Negative probes for the source-owned guidance checker."""

import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


ROOT = Path(sys.argv.pop(1)).resolve()
SPEC = importlib.util.spec_from_file_location("guidance", ROOT / "tools/check_agent_guidance.py")
GUIDANCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUIDANCE)
PARAGRAPH = "An established source owns this instruction and its explicit applicability scope. " * 4


class GuidanceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-guidance-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "skill"
        (self.root / "references").mkdir(parents=True)
        self.entry = self.root / "SKILL.md"
        self.entry.write_text("# Entry\n\nFor one scoped task, read [scope](references/scoped.md#owned-rule).\n")
        (self.root / "references/scoped.md").write_text("# Scoped\n\n## Owned Rule\n\n" + PARAGRAPH + "\n")

    def ids(self, budget=1000):
        return {row["id"] for row in GUIDANCE.check_bundle(self.root, budget)["errors"]}

    def test_valid_scoped_bundle(self):
        self.assertEqual(self.ids(), set())
        report = GUIDANCE.check_bundle(self.root, 1000)
        self.assertEqual(report["pages"], ["SKILL.md", "references/scoped.md"])

    def test_missing_reference_is_not_a_silent_partial_bundle(self):
        (self.root / "references/scoped.md").unlink()
        self.assertIn("missing-or-symlink-reference", self.ids())

    def test_orphaned_procedure_fails(self):
        (self.root / "references/forgotten.md").write_text("# Forgotten\n")
        self.assertIn("unreachable-reference", self.ids())

    def test_wrong_fragment_fails(self):
        self.entry.write_text("[scope](references/scoped.md#missing)\n")
        self.assertIn("missing-reference-anchor", self.ids())

    def test_link_outside_bundle_fails_even_when_it_exists(self):
        (self.base / "outside.md").write_text("# Outside\n")
        self.entry.write_text("[scope](../outside.md)\n")
        self.assertIn("reference-outside-owner", self.ids())

    def test_absolute_and_file_scheme_links_fail(self):
        for target in (str(self.base / "outside.md"), "file:///outside.md"):
            with self.subTest(target=target):
                self.entry.write_text(f"[scope]({target})\n")
                self.assertIn("reference-outside-owner", self.ids())

    def test_symlink_escape_fails(self):
        outside = self.base / "outside.md"
        outside.write_text("# Outside\n")
        source = self.root / "references/scoped.md"
        source.unlink()
        source.symlink_to(outside)
        self.assertIn("reference-outside-owner", self.ids())

    def test_entry_byte_budget_boundary(self):
        size = self.entry.stat().st_size
        self.assertNotIn("entry-byte-budget", self.ids(size))
        self.assertIn("entry-byte-budget", self.ids(size - 1))

    def test_duplicate_prose_fails_across_reflowed_lines(self):
        with self.entry.open("a") as stream:
            stream.write("\n" + PARAGRAPH.replace(" ", "\n") + "\n")
        self.assertIn("duplicate-prose", self.ids())

    def test_code_examples_and_frontmatter_do_not_look_like_policy_copies(self):
        original = self.entry.read_text()
        self.entry.write_text("---\ndescription: " + PARAGRAPH + "\n---\n" + original +
                              "\n```md\n[example](missing.md)\n" + PARAGRAPH + "\n```\n")
        self.assertNotIn("duplicate-prose", self.ids(2000))
        self.assertNotIn("missing-or-symlink-reference", self.ids(2000))

    def test_cyclic_reference_graph_terminates(self):
        with (self.root / "references/scoped.md").open("a") as stream:
            stream.write("\n[entry](../SKILL.md)\n")
        self.assertEqual(self.ids(), set())

    def test_machine_specific_paths_are_rejected_in_references(self):
        with (self.root / "references/scoped.md").open("a") as stream:
            stream.write("\nUse /home/operator/another-project.\n")
        self.assertIn("machine-specific-guidance", self.ids())

    def test_real_bundles_survive_independent_relocation(self):
        for name, (relative, budget) in GUIDANCE.BUNDLES.items():
            destination = self.base / name
            shutil.copytree(ROOT / relative, destination)
            with self.subTest(bundle=name):
                self.assertEqual(GUIDANCE.check_bundle(destination, budget)["errors"], [])

    def test_current_repository_policy_and_enrollment(self):
        self.assertTrue(GUIDANCE.check_repository(ROOT)["ok"])


if __name__ == "__main__":
    unittest.main()
