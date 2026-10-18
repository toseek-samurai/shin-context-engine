#!/usr/bin/env python3
"""Exercise real self-profile ownership in an isolated source copy."""

import contextlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


# Ownership is explicit in the public profiles. A separate synthetic archive
# exercises isolation without shipping any historical project corpus.
CORE_HISTORY_DOCUMENTS = {"archive-fixture.md"}


FIXTURE_TREES = (
    "docs", "src", "profiles", "integrations", "server", "observer", "shared",
)


def fixture_ignored(directory, names):
    return {
        name for name in names
        if name in {".git", "build", "generated", "__pycache__"}
        or name.startswith("build-")
        or (Path(directory) / name).is_symlink()
    }


def copy_fixture(source, destination, root_files):
    destination.mkdir()
    for name in FIXTURE_TREES:
        path = source / name
        if path.is_dir() and not path.is_symlink():
            shutil.copytree(path, destination / name, ignore=fixture_ignored)
    for relative in root_files:
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise AssertionError(f"Unsafe fixture root file: {relative}")
        current = source
        for part in path.parts:
            if part in fixture_ignored(current, [part]):
                break
            current /= part
        else:
            target = destination / path
            if current.is_file() and not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(current, target)


def unowned_machine_documents(root, core_documents, companion_documents):
    actual = {
        path.relative_to(root).as_posix()
        for path in (root / "docs/machine").glob("*.json")
        if path.is_file() and not path.is_symlink()
    }
    return actual - core_documents - companion_documents


class SelfProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="shin-self-profile-", dir=WORK_PARENT
        )
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.root = cls.work / "root"
        root_files = json.loads((SOURCE_ROOT / "profiles/shin.json").read_text())["rootFiles"]
        root_files = root_files + ["LICENSE", "NOTICE", "THIRD_PARTY.md", "CONTRIBUTING.md", "SECURITY.md", "Makefile"]
        copy_fixture(SOURCE_ROOT, cls.root, root_files)
        cls.profile = cls.read_json("profiles/shin.json")
        cls.history_documents = {"docs/archive-fixture.md"}
        (cls.root / "docs/archive-fixture.md").write_text(
            "# Synthetic archive\n\nArchived fixture recovery decisions are separate from current source.\n")
        cls.history = cls.read_json("profiles/generic-docs.json")
        cls.history.update({
            "id": "archive-fixture", "rootFiles": [], "optionalRootFiles": [],
            "documentIncludePrefixes": sorted(cls.history_documents),
            "evalCases": [{"id": "archive-note", "query": "archive-fixture.md",
                           "mustPaths": sorted(cls.history_documents), "limit": 4}],
            "packEvalCases": [{
                "id": "archive-pack", "request": {
                    "schema_version": "shin.task-request.v1", "request_id": "archive-pack",
                    "task": {"objective": "Read the isolated synthetic archive.",
                             "must": [{"id": "archive", "query": "archive-fixture.md"}]},
                    "budget": {"max_evidence_bytes": 16000, "max_records": 6,
                               "per_query_records": 3, "per_document_records": 3}},
                "mustPaths": sorted(cls.history_documents), "expectedComplete": True,
                "maxRecords": 6, "maxEvidenceBytes": 16000, "maxCompactBytes": 28000,
                "requireSerialParallelIdentity": True}],
        })
        (cls.root / "profiles/archive-fixture.json").write_text(json.dumps(cls.history))
        cls.companions = [
            cls.read_json(f"profiles/{name}.json")
            for name in ("shin-observer",)
        ]
        distribution = cls.read_json("docs/machine/portable-distribution-v1.json")
        cls.core_documents = {
            path for path in cls.profile["documentIncludePrefixes"]
            if path.startswith("docs/machine/") and path.endswith(".json")
        }
        portable = {"docs/machine/" + Path(path).name for path in distribution["layout"]["machineContracts"]}
        if not portable <= cls.core_documents:
            raise AssertionError(f"Portable contracts are outside Core ownership: {portable - cls.core_documents}")
        cls.companion_documents = {
            path for profile in cls.companions
            for path in profile["documentIncludePrefixes"]
            if path.startswith("docs/machine/") and path.endswith(".json")
        } - cls.core_documents
        cls.run_tool("build")
        cls.run_tool("profile-check", "--strict")
        cls.manifest = json.loads(
            (cls.work / "shin/docs/kb/generated/manifest.json").read_text()
        )
        cls.baseline = cls.run_tool("worktree-diff", "--strict")

    @classmethod
    def read_json(cls, path):
        return json.loads((cls.root / path).read_text())

    @classmethod
    def run_tool(cls, *arguments, profile="shin", expected=0):
        command = [
            str(TOOL), "--profile", profile,
            "--profile-dir", str(cls.root / "profiles"),
            "--root", str(cls.root), "--out-dir", str(cls.work / profile),
            *arguments,
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode != expected:
            raise AssertionError(
                f"{command}: expected exit {expected}, got {result.returncode}\n"
                f"{result.stdout}\n{result.stderr}"
            )
        return json.loads(result.stdout)

    @contextlib.contextmanager
    def changed_files(self, updates):
        originals = {}
        try:
            for relative, content in updates.items():
                path = self.root / relative
                originals[path] = path.read_bytes() if path.exists() else None
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            yield
        finally:
            for path, content in originals.items():
                if content is None:
                    path.unlink()
                else:
                    path.write_bytes(content)

    def setUp(self):
        self.assertTrue(self.run_tool("worktree-diff", "--strict")["current"])

    def assert_changed_path(self, path, category, kind="changed"):
        result = self.run_tool("worktree-diff", "--strict", expected=1)
        self.assertFalse(result["current"])
        details = [item for item in result["details"] if item.get("path") == path]
        self.assertEqual(len(details), 1, result)
        self.assertEqual(details[0]["category"], category)
        self.assertEqual(details[0]["change"], kind)
        return result

    def test_all_contracted_core_documents_retained(self):
        indexed = {source["path"] for source in self.manifest["sources"]}
        self.assertEqual(
            {path for path in indexed if path.startswith("docs/machine/")},
            self.core_documents,
        )
        self.assertNotIn("docs/kb-context-engine-improvement-notes.md", indexed)
        self.assertFalse(self.history_documents & indexed)
        self.assertIn("docs/architecture.md", indexed)
        self.assertTrue(set(self.profile["rootFiles"]) <= indexed)
        required_paths = {
            path
            for case in self.profile["evalCases"] + self.profile["packEvalCases"]
            for field in ("mustPaths", "mustTopPaths")
            for path in case.get(field, [])
        }
        facts = {source["path"] for source in self.manifest["source_fact_sources"]}
        self.assertFalse(required_paths - indexed - facts)
        self.assertFalse(self.companion_documents & indexed)
        self.assertEqual(self.manifest["source_fact_counts"]["capped_files"], 0)
        self.assertEqual(self.manifest["source_fact_counts"]["skipped_large_files"], 0)

    def test_machine_document_inventory_has_explicit_owners(self):
        self.assertEqual(
            unowned_machine_documents(self.root, self.core_documents, self.companion_documents),
            set(), "Classify new machine documents before changing profile scope",
        )
        actual = {
            path.relative_to(self.root).as_posix()
            for path in (self.root / "docs/machine").glob("*.json")
        }
        self.assertEqual(actual, self.core_documents | self.companion_documents,
                         "Classify new machine documents before changing profile scope")
        self.assertEqual(self.profile["sourceFactRoots"],
                         ["src", "shared/windows-io/src", "integrations/hermes/plugins/shin-context"])
        self.assertIn("shared/windows-io/src/file.cpp",
                      {source["path"] for source in self.manifest["source_fact_sources"]})

    def test_companion_changes_preserve_core_currentness_and_snapshot(self):
        paths = self.companion_documents | {
            "profiles/shin-observer.json",
            "profiles/shin-observer-tests.json",
            "observer/src/main.cpp",
            "shared/observer-protocol/src/observer_protocol.cpp",
        }
        updates = {path: (self.root / path).read_bytes() + b"\n" for path in paths}
        for component in ("observer",):
            updates[f"docs/machine/shin-{component}-new-fixture-v1.json"] = (
                b'{"schemaVersion":"fixture.v1","summary":"New companion contract"}\n'
            )
        for directory in ("observer/src", "shared/observer-protocol/src"):
            updates[f"{directory}/self_profile_fixture.cpp"] = (
                b"int companion_fixture() { return 1; }\n"
            )
        with self.changed_files(updates):
            result = self.run_tool("worktree-diff", "--strict")
            self.assertTrue(result["current"])
            self.assertEqual(result["summary"]["total_changes"], 0)
            self.assertEqual(result["worktree"]["worktree_id"],
                             self.baseline["worktree"]["worktree_id"])
            self.run_tool("build")
            rebuilt = self.run_tool("worktree-diff", "--strict")
            self.assertEqual(rebuilt["snapshot"]["snapshot_id"],
                             self.baseline["snapshot"]["snapshot_id"])

    def test_core_document_changes_drift(self):
        for path in ("docs/machine/retrieval-evidence-v1.json",
                     "docs/machine/shin-warm-snapshot-v1.json"):
            with self.subTest(path=path), self.changed_files(
                {path: (self.root / path).read_bytes() + b"\n"}
            ):
                self.assert_changed_path(path, "document")

    def test_core_source_edits_and_additions_drift(self):
        for path in ("src/generated_snapshot.cpp", "shared/windows-io/src/file.cpp"):
            with self.subTest(path=path), self.changed_files({path: (self.root / path).read_bytes() + b"\n"}):
                self.assert_changed_path(path, "source-fact")
        for directory in ("src", "shared/windows-io/src"):
            path = f"{directory}/self_profile_fixture.cpp"
            with self.subTest(path=path), self.changed_files({path: b"int core_fixture() { return 1; }\n"}):
                self.assert_changed_path(path, "source-fact", "added")

    def test_shared_entrypoints_still_drift(self):
        for path in ("AGENTS.md", "README.md"):
            with self.subTest(path=path), self.changed_files(
                {path: (self.root / path).read_bytes() + b"\n"}
            ):
                self.assert_changed_path(path, "document")

    def test_new_machine_document_requires_explicit_classification(self):
        path = "docs/machine/unclassified-fixture-v1.json"
        with self.changed_files({path: b'{"summary":"Unclassified document"}\n'}):
            self.assertEqual(
                unowned_machine_documents(self.root, self.core_documents, self.companion_documents),
                {path},
            )
            result = self.run_tool("worktree-diff", "--strict")
            self.assertTrue(result["current"])
            self.assertEqual(result["summary"]["total_changes"], 0)

    def test_fixture_copy_excludes_private_build_and_symlink_paths(self):
        with tempfile.TemporaryDirectory(dir=self.work) as temporary:
            source = Path(temporary) / "source"
            source.mkdir()
            kept = {"README.md", "docs/machine/contract.json", "src/example.cpp"}
            excluded = {
                "private-work/note.txt", "build/result.json", "build-cmake-debug/cache.txt",
                "shared/build-cmake-debug/cache.txt", "observer/build-private/cache.txt",
                "docs/kb/generated/manifest.json", "profiles/.git/config",
            }
            for relative in kept | excluded:
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture\n")
            (source / "src/private-link").symlink_to(source / "private-work", target_is_directory=True)
            (source / "src/file-link.cpp").symlink_to(source / "README.md")
            (source / "root-link.md").symlink_to(source / "README.md")
            (source / "routing").symlink_to(source / "private-work", target_is_directory=True)
            destination = Path(temporary) / "copy"
            copy_fixture(source, destination, ["README.md", "root-link.md"])
            self.assertFalse(any(path.is_symlink() for path in destination.rglob("*")))
            self.assertEqual(
                {path.relative_to(destination).as_posix()
                 for path in destination.rglob("*") if path.is_file()},
                kept,
            )

    def test_companion_routes_observe_their_own_documents(self):
        for profile, path in (
            ("shin-observer", "docs/machine/shin-local-observer-v1.json"),
        ):
            with self.subTest(profile=profile):
                self.run_tool("build", profile=profile)
                with self.changed_files({path: (self.root / path).read_bytes() + b"\n"}):
                    result = self.run_tool("worktree-diff", "--strict",
                                           profile=profile, expected=1)
                    self.assertFalse(result["current"])
                    self.assertEqual(result["summary"]["documents_changed"], 1)
                    self.assertEqual([item["path"] for item in result["details"]], [path])
                    self.assertTrue(self.run_tool("worktree-diff", "--strict")["current"])

    def test_history_changes_and_failures_do_not_change_working_snapshot(self):
        declared = set(self.history["documentIncludePrefixes"])
        self.assertEqual(declared, self.history_documents)
        self.run_tool("build", profile="archive-fixture")
        self.run_tool("eval", "--strict", profile="archive-fixture")
        self.run_tool("pack-eval", "--strict", profile="archive-fixture")
        for path in declared:
            with self.subTest(path=path), self.changed_files({path: (self.root / path).read_bytes() + b"\n"}):
                self.assertTrue(self.run_tool("worktree-diff", "--strict")["current"])
                self.assertFalse(self.run_tool("worktree-diff", "--strict", profile="archive-fixture", expected=1)["current"])
                self.run_tool("build")
                self.assertEqual(self.run_tool("worktree-diff", "--strict")["snapshot"]["snapshot_id"],
                                 self.baseline["snapshot"]["snapshot_id"])

        changed = json.loads(json.dumps(self.history))
        changed["evalCases"][0]["query"] = "nonexistentarchaeologyprobe"
        with self.changed_files({"profiles/archive-fixture.json": json.dumps(changed).encode()}):
            self.run_tool("build", profile="archive-fixture")
            self.assertFalse(self.run_tool("eval", "--strict", profile="archive-fixture", expected=1)["ok"])
            self.assertTrue(self.run_tool("worktree-diff", "--strict")["current"])
            self.run_tool("build")
            self.assertEqual(self.run_tool("worktree-diff", "--strict")["snapshot"]["snapshot_id"],
                             self.baseline["snapshot"]["snapshot_id"])


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: run_self_profile_tests.py <shin-context> <source-root> [work-parent]")
    TOOL = Path(sys.argv[1]).resolve(strict=True)
    SOURCE_ROOT = Path(sys.argv[2]).resolve(strict=True)
    WORK_PARENT = Path(sys.argv[3]).resolve(strict=True) if len(sys.argv) == 4 else None
    unittest.main(argv=[sys.argv[0]], verbosity=2)
