#!/usr/bin/env python3
"""Native Observer source-query integration with isolated, empty-PATH fixtures."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from integration_test import Fixture, write_private_json

CORE = OBSERVER = None


def decode_source_view(raw):
    text = raw.decode("utf-8")
    lines = text.split("\n")
    if not text.startswith("SHIN ") or not text.endswith("\n") or not lines[-2].startswith("END "):
        raise ValueError("Missing source-view framing")
    header, receipt = json.loads(lines[0][5:]), json.loads(lines[-2][4:])
    schema = header["schema_version"]
    if schema not in {"shin.source-view.v1", "shin.source-view.v2"}:
        raise ValueError("Unknown source view")
    columns = header.get("locator_columns")
    if schema == "shin.source-view.v2" and (not isinstance(columns, list) or not columns or
            any(not isinstance(c, str) for c in columns) or len(set(columns)) != len(columns)):
        raise ValueError("Invalid source locator columns")
    payload = text[:text.rfind("\nEND ") + 1].encode()
    if receipt["payload_bytes"] != len(payload) or receipt["sha256"] != hashlib.sha256(payload).hexdigest():
        raise ValueError("Source-view transport changed")
    if len(raw) > header["max_bytes"] or any(len(line.encode()) > 1900 for line in lines):
        raise ValueError("Source-view delivery budget")
    metadata, bodies, active = {}, {}, None
    for line in lines[1:-2]:
        if line.startswith("SOURCE "):
            _, number, record = line.split(" ", 2)
            locator = json.loads(record)
            if schema == "shin.source-view.v2":
                if not isinstance(locator, list) or len(locator) != len(columns):
                    raise ValueError("Invalid source locator row")
                locator = {key: value for key, value in zip(columns, locator) if value is not None}
            metadata[int(number)] = locator
        elif line.startswith("TEXT "):
            active = int(line[5:])
            if active in bodies or active not in metadata:
                raise ValueError("Unexpected source body")
            bodies[active] = b""
        elif re.match(r"L\d+\+", line):
            location, segment = line.split(" ", 1)
            number, offset = map(int, location[1:].split("+"))
            if active is None or len(bodies[active]) != offset or number != metadata[active]["start_line"] + bodies[active].count(b"\n"):
                raise ValueError("Source-view coordinates")
            bodies[active] += json.loads(segment).encode()
    if sorted(bodies) != receipt["shown"] or len(metadata) != header["bodies"]:
        raise ValueError("Source-view inventory")
    return header, metadata, bodies, receipt


class SourceQueryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shin-source-query-")
        self.addCleanup(self.temporary.cleanup)
        self.fixture = Fixture(Path(self.temporary.name).resolve(), "project")
        f = self.fixture
        (f.root / "src").mkdir()
        (f.root / "src/cache.sql").write_text("SELECT persistentBoundary FROM private_workspace;\n")
        profile = json.loads(f.profile_path.read_text())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".sql"])
        f.profile_path.write_text(json.dumps(profile))
        self.build()

    def invoke(self, *args, ok=True):
        process = subprocess.run([str(OBSERVER), *args], capture_output=True, text=True, timeout=30,
                                 env={"PATH": "", "LC_ALL": "C"})
        self.assertEqual(process.returncode == 0, ok, process.stdout + process.stderr)
        self.assertFalse(process.stderr if ok else process.stdout)
        result = json.loads(process.stdout if ok else process.stderr)
        self.assertEqual(result["ok"], ok)
        return result["result"] if ok else result["error"]

    def query(self, query="persistentBoundary", ok=True):
        return self.invoke("source-query", "--config", str(self.fixture.config), "--query", query, ok=ok)

    def sample(self):
        return self.invoke("sample", "--config", str(self.fixture.config))

    def context(self, query="persistentBoundary", ok=True):
        return self.invoke("source-context", "--config", str(self.fixture.config), "--query", query, ok=ok)

    def inspect(self, snapshot, path="src/cache.sql", ok=True):
        return self.invoke("source-inspect", "--config", str(self.fixture.config),
                           "--path", path, "--snapshot", snapshot, ok=ok)

    def read(self, snapshot, paths=None, ids=None, ok=True):
        request = self.fixture.base / "read.json"
        selection = {"record_ids": ids} if ids is not None else {"file_paths": paths or ["src/cache.sql"]}
        write_private_json(request, {"schema_version": "shin.source-read-request.v1" if ids is not None else
                                     "shin.source-read-request.v2", "snapshot_id": snapshot, **selection})
        return self.invoke("source-read", "--config", str(self.fixture.config), "--input", str(request), ok=ok)

    def build(self):
        f = self.fixture
        result = subprocess.run([str(CORE), "--root", str(f.root), "--out-dir", str(f.out),
                                 "--profile-dir", str(f.profiles), "--profile", "observer-fixture", "build"],
                                capture_output=True, text=True, timeout=30, env={"PATH": "", "LC_ALL": "C"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unattached_refused_without_cache(self):
        self.assertEqual(self.query(ok=False)["code"], "state-unavailable")
        self.assertFalse((self.fixture.state / "source-cache").exists())

    def test_restart_parity_and_no_state_or_query_persistence(self):
        self.sample()
        state = (self.fixture.state / "state.json").read_bytes()
        before = {str(p): p.read_bytes() for p in self.fixture.out.rglob("*") if p.is_file()}
        first, second = self.query(), self.query()
        self.assertEqual(first["source_candidates"]["cache"]["status"], "created")
        self.assertEqual(second["source_candidates"]["cache"]["status"], "restored")
        self.assertEqual(second["source_candidates"]["reuse"]["index_builds"], 0)
        a, b = (x["source_candidates"]["results"][0]["candidates"] for x in (first, second))
        self.assertEqual(a, b)
        self.assertEqual(a[0]["path"], "src/cache.sql")
        self.assertTrue(second["live_source_verified"])
        self.assertFalse(second["readiness_assessed"])
        marker = "neverPersistQueryMarkerUnique12345"
        self.query(marker)
        self.assertEqual(state, (self.fixture.state / "state.json").read_bytes())
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.fixture.out.rglob("*") if p.is_file()})
        for p in self.fixture.state.rglob("*"):
            if p.is_file():
                self.assertNotIn(marker.encode(), p.read_bytes())

    def test_saved_current_does_not_authorize_changed_source(self):
        self.sample()
        self.query()
        (self.fixture.root / "src/cache.sql").write_text("SELECT changedBoundary;\n")
        self.query(ok=False)

    def test_explicit_publication_recovers_despite_historical_stale_state(self):
        self.sample()
        self.query()
        (self.fixture.root / "src/cache.sql").write_text("SELECT changedBoundary;\n")
        self.assertEqual(self.sample()["observation"]["status"], "stale")
        self.build()
        result = self.query("changedBoundary")
        self.assertEqual(result["source_candidates"]["cache"]["status"], "rebuilt")
        self.assertEqual(result["source_candidates"]["results"][0]["candidates"][0]["path"], "src/cache.sql")

    def test_foreign_binding_refused(self):
        self.sample()
        config = self.fixture.config_data()
        config["project_id"] = "another-project"
        write_private_json(self.fixture.config, config)
        self.assertEqual(self.query(ok=False)["code"], "state-binding-mismatch")

    def test_replaced_root_refused_even_with_equal_content(self):
        self.sample()
        old = self.fixture.base / "old-source"
        self.fixture.root.rename(old)
        shutil.copytree(old, self.fixture.root)
        self.assertEqual(self.query(ok=False)["code"], "root-identity-changed")

    def test_snapshot_limits_not_bypassed(self):
        self.sample()
        config = self.fixture.config_data()
        config.update(max_snapshot_bytes=1, max_file_bytes=1)
        write_private_json(self.fixture.config, config)
        self.query(ok=False)
        self.assertFalse((self.fixture.state / "source-cache").exists())

    def test_arguments_bounded(self):
        self.sample()
        self.assertEqual(self.query("", ok=False)["code"], "invalid-source-query")
        self.assertEqual(self.query("x" * 4097, ok=False)["code"], "invalid-source-query")
        self.invoke("source-query", "--config", str(self.fixture.config), ok=False)
        self.invoke("source-query", "--config", str(self.fixture.config), "--query", "one", "--query", "two", ok=False)

    def test_context_requires_binding_and_returns_exact_source_without_mutation(self):
        self.assertEqual(self.context(ok=False)["code"], "state-unavailable")
        self.sample()
        before = (self.fixture.state / "state.json").read_bytes()
        result = self.context()
        self.assertEqual(result["schema_version"], "shin.observer-source-context.v1")
        native = result["source_context"]
        self.assertEqual(native["schema_version"], "shin.source-context.v1")
        self.assertEqual(native["context"]["bodies"][0]["text"], (self.fixture.root / "src/cache.sql").read_text())
        self.assertFalse(native["context"]["answer_complete"])
        self.assertEqual(before, (self.fixture.state / "state.json").read_bytes())
        (self.fixture.root / "src/cache.sql").write_text("SELECT changedBoundary;\n")
        self.context(ok=False)

    def test_context_arguments_bounded(self):
        self.sample()
        self.assertEqual(self.context("", ok=False)["code"], "invalid-source-query")
        self.invoke("source-context", "--config", str(self.fixture.config), ok=False)
        self.invoke("source-context", "--config", str(self.fixture.config), "--query", "one", "--query", "two", ok=False)

    def compact(self, command="source-context", extra=None, budget=8192):
        args = extra if extra is not None else ["--query", "persistentBoundary"]
        process = subprocess.run([str(OBSERVER), command, "--config", str(self.fixture.config), *args,
            "--projection", "compact", "--max-bytes", str(budget)], capture_output=True, timeout=30,
            env={"PATH": "", "LC_ALL": "C"})
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertFalse(process.stderr)
        return process.stdout, decode_source_view(process.stdout)

    def test_compact_context_preserves_exact_source_and_binding(self):
        self.sample()
        native = self.context()["source_context"]
        state = (self.fixture.state / "state.json").read_bytes()
        raw, (header, metadata, bodies, receipt) = self.compact()
        self.assertEqual(header["schema_version"], "shin.source-view.v2")
        self.assertEqual(header["snapshot_id"], native["snapshot_id"])
        self.assertFalse(header["answer_complete"])
        self.assertEqual(len(metadata), len(native["context"]["bodies"]))
        for number, text in bodies.items():
            source = native["context"]["bodies"][number - 1]
            self.assertEqual(text, source["text"].encode())
            self.assertEqual(metadata[number]["span_hash"], source["span_hash"])
            self.assertEqual(metadata[number], {key: source[key] for key in header["locator_columns"] if key in source})
        self.assertTrue(receipt["all_native_bodies_shown"])
        self.assertEqual(state, (self.fixture.state / "state.json").read_bytes())
        for altered in (raw[:-100], raw + b"extra\n", raw.replace(b"persistentBoundary", b"differentBoundary")):
            with self.assertRaises(ValueError):
                decode_source_view(altered)
        lines = raw.decode().split("\n")
        source_index = next(i for i, line in enumerate(lines) if line.startswith("SOURCE 1 "))
        row = json.loads(lines[source_index].split(" ", 2)[2])
        lines[source_index] = "SOURCE 1 " + json.dumps(row[:-1])
        payload = ("\n".join(lines[:-2]) + "\n").encode()
        malformed_receipt = {**receipt, "payload_bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
        with self.assertRaisesRegex(ValueError, "locator row"):
            decode_source_view(payload + ("END " + json.dumps(malformed_receipt) + "\n").encode())

    def test_compact_read_omits_whole_over_budget_body_and_can_deliver_larger_view(self):
        path = self.fixture.root / "src/cache.sql"
        path.write_text("SELECT persistentBoundary;\n" * 120)
        self.build()
        self.sample()
        snapshot = self.query()["source_candidates"]["snapshot_id"]
        self.read(snapshot)
        args = ["--input", str(self.fixture.base / "read.json")]
        _, (_, metadata, bodies, receipt) = self.compact("source-read", args, 2048)
        self.assertEqual(metadata[1]["path"], "src/cache.sql")
        self.assertEqual(bodies, {})
        self.assertEqual(receipt["omitted_for_view_budget"], [1])
        _, (_, _, bodies, receipt) = self.compact("source-read", args, 16384)
        self.assertEqual(bodies[1], path.read_bytes())
        self.assertEqual(receipt["omitted_for_view_budget"], [])
        path.write_text("SELECT changedBoundary;\n")
        self.invoke("source-read", "--config", str(self.fixture.config), *args,
                    "--projection", "compact", ok=False)

    def test_compact_arguments_do_not_apply_to_unrelated_commands(self):
        self.sample()
        for args in (["--max-bytes", "2048"], ["--projection", "unknown"],
                     ["--projection", "compact", "--max-bytes", "2047"],
                     ["--projection", "compact", "--max-bytes", "32769"]):
            self.invoke("source-context", "--config", str(self.fixture.config), "--query", "persistentBoundary", *args, ok=False)
        self.invoke("status", "--config", str(self.fixture.config), "--projection", "compact", ok=False)
        self.invoke("source-query", "--config", str(self.fixture.config), "--query", "x", "--projection", "compact", ok=False)

    def test_compact_inventory_preserves_ids_and_refuses_stale_source(self):
        self.sample()
        snapshot = self.context()["source_context"]["snapshot_id"]
        native = self.inspect(snapshot)["source_inspection"]
        args = [str(OBSERVER), "source-inspect", "--config", str(self.fixture.config),
                "--path", "src/cache.sql", "--snapshot", snapshot, "--projection", "compact", "--max-bytes", "2048"]
        result = subprocess.run(args, capture_output=True, timeout=30, env={"PATH": "", "LC_ALL": "C"})
        self.assertEqual(result.returncode, 0, result.stderr)
        view = json.loads(result.stdout)
        self.assertEqual(view["schema_version"], "shin.source-inventory-view.v1")
        self.assertEqual(view["snapshot_id"], snapshot)
        self.assertEqual(view["native_coverage"], native["coverage"])
        self.assertEqual(len(view["records"]) + view["view"]["omitted_records"], len(native["records"]))
        self.assertLessEqual(len(result.stdout), 2048)
        for compact, original in zip(view["records"], native["records"]):
            self.assertEqual(compact.get("id"), original.get("id"))
        (self.fixture.root / "src/cache.sql").write_text("SELECT changedBoundary;\n")
        self.assertNotEqual(subprocess.run(args, capture_output=True).returncode, 0)

    def test_followup_whole_file_shares_context_snapshot_without_mutation(self):
        self.sample()
        snapshot = self.context()["source_context"]["snapshot_id"]
        before = {str(p): p.read_bytes() for base in (self.fixture.out, self.fixture.state)
                  for p in base.rglob("*") if p.is_file()}
        inspection = self.inspect(snapshot)
        native = inspection["source_inspection"]
        self.assertEqual(native["snapshot_id"], snapshot)
        self.assertTrue(native["index_freshness"]["current"])
        self.assertEqual(native["path"], "src/cache.sql")
        # Unsupported syntax can have no declarations but still be an enrolled
        # file; file reading must not depend on inventing a record identifier.
        for result in (self.read(snapshot), self.read(snapshot)):
            self.assertEqual(result["schema_version"], "shin.observer-source-read.v1")
            self.assertFalse(result["index_mutated"])
            read = result["source_read"]
            self.assertTrue(read["complete"])
            self.assertEqual(read["snapshot_id"], snapshot)
            self.assertEqual(read["evidence"]["bodies"][0]["text"], (self.fixture.root / "src/cache.sql").read_text())
        self.assertEqual(before, {str(p): p.read_bytes() for base in (self.fixture.out, self.fixture.state)
                                 for p in base.rglob("*") if p.is_file()})

    def test_followup_unattached_wrong_snapshot_and_stale_refused(self):
        wrong = "fnv1a64:0000000000000000"
        self.assertEqual(self.inspect(wrong, ok=False)["code"], "state-unavailable")
        self.assertEqual(self.read(wrong, ok=False)["code"], "state-unavailable")
        self.sample()
        snapshot = self.context()["source_context"]["snapshot_id"]
        self.assertEqual(self.inspect(wrong, ok=False)["code"], "source-snapshot-mismatch")
        self.assertEqual(self.read(wrong, ok=False)["code"], "source-read-snapshot-mismatch")
        (self.fixture.root / "src/cache.sql").write_text("SELECT staleImplementation;\n")
        self.inspect(snapshot, ok=False)
        self.read(snapshot, ok=False)
        self.build()
        self.inspect(snapshot, ok=False)
        self.read(snapshot, ok=False)

    def test_followup_paths_requests_and_input_files_bounded(self):
        self.sample()
        snapshot = self.context()["source_context"]["snapshot_id"]
        for path in ("../outside.sql", "/outside.sql", "src/../src/cache.sql"):
            self.inspect(snapshot, path, ok=False)
            self.read(snapshot, paths=[path], ok=False)
        self.assertEqual(self.read(snapshot, paths=["README.md"], ok=False)["code"], "source-read-unknown-file")
        self.inspect("not-a-snapshot", ok=False)
        self.read(snapshot, ids=["unknown-record"], ok=False)
        request = self.fixture.base / "read.json"
        for content in ('{"schema_version":"one","schema_version":"two"}', "x" * (128 * 1024 + 1)):
            request.write_text(content)
            request.chmod(0o600)
            self.invoke("source-read", "--config", str(self.fixture.config), "--input", str(request), ok=False)
        link = self.fixture.base / "linked-request.json"
        link.symlink_to(request)
        self.invoke("source-read", "--config", str(self.fixture.config), "--input", str(link), ok=False)
        self.invoke("source-inspect", "--config", str(self.fixture.config), "--path", "src/cache.sql", ok=False)
        self.invoke("source-read", "--config", str(self.fixture.config), "--query", "wrong-option", ok=False)

    def test_followup_exposes_complete_declaration_beyond_first_fragment(self):
        f = self.fixture
        content = "int selectedBoundary() {\n" + "".join(f"  int item{i} = {i};\n" for i in range(100)) + "  return lateImplementationNeedle;\n}\n"
        (f.root / "src/late.cpp").write_text(content)
        profile = json.loads(f.profile_path.read_text())
        profile["sourceFactExtensions"].append(".cpp")
        f.profile_path.write_text(json.dumps(profile))
        self.build()
        self.sample()
        first = self.context("selectedBoundary")["source_context"]
        self.assertFalse(any("lateImplementationNeedle" in b["text"] for b in first["context"]["bodies"]))
        inspected = self.inspect(first["snapshot_id"], "src/late.cpp")["source_inspection"]
        ids = [r["id"] for r in inspected["records"] if r.get("symbol_name") == "selectedBoundary"]
        self.assertTrue(ids)
        result = self.read(first["snapshot_id"], ids=ids[:1])["source_read"]
        self.assertTrue(result["complete"])
        self.assertTrue(any("lateImplementationNeedle" in b["text"] for b in result["evidence"]["bodies"]))

    def test_followup_whole_file_redaction_and_oversize_are_explicit(self):
        path = self.fixture.root / "src/cache.sql"
        path.write_text("SELECT persistentBoundary;\n-- password = 'privateSyntheticPassword12345'\n")
        self.build()
        self.sample()
        snapshot = self.query()["source_candidates"]["snapshot_id"]
        result = self.read(snapshot)["source_read"]
        self.assertFalse(result["complete"])
        self.assertIsNone(result["evidence"]["bodies"][0]["text"])
        path.write_text("-- " + "benign " * 5000 + "\nSELECT persistentBoundary;\n")
        self.build()
        snapshot = self.query()["source_candidates"]["snapshot_id"]
        result = self.read(snapshot)["source_read"]
        self.assertFalse(result["complete"])
        self.assertEqual(result["evidence"]["bodies"], [])
        self.assertEqual(result["evidence"]["omissions"][0]["reason"], "serialized-evidence-budget")

    def test_followup_replaced_root_refused(self):
        self.sample()
        snapshot = self.context()["source_context"]["snapshot_id"]
        old = self.fixture.base / "old-source"
        self.fixture.root.rename(old)
        shutil.copytree(old, self.fixture.root)
        self.assertEqual(self.inspect(snapshot, ok=False)["code"], "root-identity-changed")
        self.assertEqual(self.read(snapshot, ok=False)["code"], "root-identity-changed")

    def test_new_and_deleted_files_without_vcs_or_with_svn_marker(self):
        for metadata in (None, ".svn"):
            with self.subTest(metadata=metadata):
                if metadata:
                    (self.fixture.root / metadata).mkdir()
                self.assertFalse((self.fixture.root / ".git").exists())
                self.sample()
                added = self.fixture.root / "src/uncommitted.sql"
                added.write_text("SELECT newlyAddedSourceNeedle;\n")
                self.query("newlyAddedSourceNeedle", ok=False)
                self.assertEqual(self.sample()["observation"]["changes"]["source_facts_added"], 1)
                self.build()
                result = self.query("newlyAddedSourceNeedle")
                self.assertEqual(result["source_candidates"]["results"][0]["candidates"][0]["path"], "src/uncommitted.sql")
                added.unlink()
                self.query("newlyAddedSourceNeedle", ok=False)
                self.assertEqual(self.sample()["observation"]["changes"]["source_facts_removed"], 1)
                self.build()
                self.assertEqual(self.query("newlyAddedSourceNeedle")["source_candidates"]["results"][0]["candidates"], [])

    def test_git_staged_modified_and_untracked_are_filesystem_sources(self):
        git = shutil.which("git")
        self.assertIsNotNone(git, "Git is required only to arrange this fixture, never by Shin")
        env = {"PATH": "", "HOME": str(self.fixture.base), "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": "/dev/null"}
        def setup_git(*args):
            result = subprocess.run([git, "-C", str(self.fixture.root), *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout
        setup_git("init", "--quiet", "--template=")
        setup_git("add", "README.md", "src/cache.sql")
        setup_git("-c", "user.name=Shin Fixture", "-c", "user.email=fixture@invalid",
                  "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
                  "-c", "maintenance.auto=false", "commit", "--quiet", "-m", "fixture")
        self.sample()
        (self.fixture.root / "src/cache.sql").write_text("SELECT modifiedUncommittedNeedle;\n")
        (self.fixture.root / "src/staged.sql").write_text("SELECT stagedUncommittedNeedle;\n")
        setup_git("add", "src/staged.sql")
        (self.fixture.root / "src/untracked.sql").write_text("SELECT untrackedSourceNeedle;\n")
        status = setup_git("status", "--porcelain=v1", "--untracked-files=all", "--", "src")
        self.assertIn(" M src/cache.sql", status)
        self.assertIn("A  src/staged.sql", status)
        self.assertIn("?? src/untracked.sql", status)
        git_index = (self.fixture.root / ".git/index").read_bytes()
        self.query(ok=False)
        changes = self.sample()["observation"]["changes"]
        self.assertEqual(changes["source_facts_added"], 2)
        self.assertEqual(changes["source_facts_changed"], 1)
        self.build()
        for query, expected in (("modifiedUncommittedNeedle", "cache.sql"),
                                ("stagedUncommittedNeedle", "staged.sql"),
                                ("untrackedSourceNeedle", "untracked.sql")):
            with self.subTest(query=query):
                candidates = self.query(query)["source_candidates"]["results"][0]["candidates"]
                self.assertEqual(candidates[0]["path"], "src/" + expected)
        self.assertEqual(git_index, (self.fixture.root / ".git/index").read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--observer", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    CORE, OBSERVER = args.core.resolve(strict=True), args.observer.resolve(strict=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SourceQueryTests))
    report = {"schema_version": "shin.observer-source-query-tests.v1", "ok": result.wasSuccessful(),
              "tests": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
              "core_sha256": hashlib.sha256(CORE.read_bytes()).hexdigest(),
              "observer_sha256": hashlib.sha256(OBSERVER.read_bytes()).hexdigest()}
    with args.report.open("x") as output:
        json.dump(report, output, sort_keys=True)
        output.write("\n")
    raise SystemExit(0 if result.wasSuccessful() else 1)
