"""Hermetic CLI source-body contract, failure atomicity and read-only checks."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = str(Path(sys.argv.pop(1)).resolve())


def state(root):
    result = {}
    for path in sorted(root.rglob("*")):
        info = path.lstat()
        result[str(path.relative_to(root))] = (
            info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None)
    return result


class SourceReadCliTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="shin-read-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "source"
        self.out = self.base / "index"
        self.profiles = self.base / "profiles"
        (self.root / "src").mkdir(parents=True)
        self.profiles.mkdir()
        (self.root / "README.md").write_text("# Fixture\n\nSource body transport.\n")
        self.source = self.root / "src/example.cpp"
        self.body = b'int example() {\r\n return 3; // "quoted" and \\tab\r\n}'
        self.source.write_bytes(self.body)
        (self.profiles / "read-fixture.json").write_text(json.dumps({
            "schemaVersion": "shin.profile.v1", "id": "read-fixture", "sourceLabel": "Read fixture",
            "rootFiles": ["README.md"], "sourceExtensions": [".md"],
            "sourceFactRoots": ["src"], "sourceFactExtensions": [".cpp"],
            "sourceFactExtractors": ["language"], "sourceFactMaxFiles": 16,
            "sourceFactMaxFileBytes": 2097152, "agentStartPaths": ["README.md"], "defaultTopic": "fixture"}))
        self.prefix = [TOOL, "--root", str(self.root), "--out-dir", str(self.out),
                       "--profile", "read-fixture", "--profile-dir", str(self.profiles)]
        self.call("build")
        outline = self.call("inspect", "src/example.cpp", "--require-current")
        self.request = {"schema_version": "shin.source-read-request.v1",
                        "snapshot_id": outline["snapshot_id"],
                        "record_ids": [row["id"] for row in outline["records"] if row.get("symbol_name") == "example"]}
        self.assertEqual(len(self.request["record_ids"]), 1)

    def call(self, *args, input=None, code=0):
        result = subprocess.run([*self.prefix, *args], input=input, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, code, (result.stdout + result.stderr).decode(errors="replace"))
        if code:
            self.assertEqual(result.stdout, b"", "a refused read must not leak earlier bodies")
            error = json.loads(result.stderr)
            self.assertFalse(error["ok"])
            return error
        self.assertEqual(result.stderr, b"")
        return json.loads(result.stdout)

    def read(self, request=None, code=0):
        return self.call("read", "--input", "-", input=json.dumps(request or self.request).encode(), code=code)

    def test_exact_body_file_stdin_and_read_only(self):
        before = state(self.root), state(self.out)
        output = self.read()
        self.assertEqual(output["schema_version"], "shin.source-read.v1")
        self.assertTrue(output["complete"])
        self.assertEqual(output["evidence"]["bodies"][0]["text"].encode(), self.body)
        self.assertEqual(before, (state(self.root), state(self.out)))
        request_path = self.base / "request.json"
        request_path.write_text(json.dumps(self.request))
        self.assertEqual(output, self.call("read", "--input", str(request_path)))

    def test_unknown_id_refuses_entire_batch(self):
        request = {**self.request, "record_ids": self.request["record_ids"] + ["absent"]}
        self.read(request, code=2)

    def test_stale_source_and_wrong_snapshot_have_no_body(self):
        self.read({**self.request, "snapshot_id": "fnv1a64:0000000000000000"}, code=1)
        self.source.write_bytes(self.body.replace(b"3", b"4"))
        self.read(code=1)

    def test_input_bounds_duplicates_and_options(self):
        self.call("read", "--input", "-", input=b" " * (128 * 1024 + 1), code=2)
        self.call("read", "--input", "-", input=b'{"schema_version":1,"schema_version":2}', code=2)
        self.call("read", "--input", "-", "--require-current", input=b"{}", code=2)
        self.call("--source-scan", "read", "--input", "-", input=json.dumps(self.request).encode(), code=2)

    def test_suppression_comment_and_late_secret(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('int example() {\n/*' + "x" * 5000 + '*/\n'
                               'const char* value = "' + secret + '"; // gitleaks:allow\nreturn 1;\n}\n')
        self.call("build")
        outline = self.call("inspect", "src/example.cpp", "--require-current")
        self.assertEqual(outline["records"], [], "quarantined files publish no searchable records")
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": outline["snapshot_id"],
                   "file_paths": ["src/example.cpp"]}
        output = self.read(request)
        self.assertFalse(output["complete"])
        self.assertNotIn(secret, json.dumps(output))
        self.assertIsNone(output["evidence"]["bodies"][0]["text"])

    def test_query_to_whole_file_without_inspect(self):
        before = state(self.root), state(self.out)
        found = self.call("brief", "example", "--projection", "compact", "--require-current")
        paths = [row["path"] for row in found["verification_paths"]]
        self.assertIn("src/example.cpp", paths)
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": found["snapshot_id"],
                   "file_paths": ["src/example.cpp"]}
        output = self.read(request)
        self.assertEqual(output["schema_version"], "shin.source-read.v2")
        self.assertTrue(output["complete"])
        self.assertEqual(output["evidence"]["bodies"][0]["text"].encode(), self.body)
        self.assertEqual(before, (state(self.root), state(self.out)))
        self.read({**request, "file_paths": ["src/example.cpp", "README.md"]}, code=2)
        self.read({**request, "record_ids": self.request["record_ids"]}, code=2)

    def test_quarantine_keeps_safe_source_available_and_recovers(self):
        secret = "sk-" + "Q7x9" * 16
        bad = self.root / "src/unsafe.cpp"
        bad.write_text('const char* token = "' + secret + '"; // gitleaks:allow\n')
        document = self.root / "docs/unsafe.md"
        document.parent.mkdir()
        document.write_text('# Sensitive configuration\n\napi_key = "' + secret + '"\n')
        self.call("build")
        doctor = self.call("doctor", "--strict")
        self.assertEqual(doctor["counts"]["quarantined_files"], 2)
        self.assertEqual(doctor["counts"]["errors"], 0)
        for path in (self.out / "docs/kb/generated").iterdir():
            if path.is_file():
                self.assertNotIn(secret.encode(), path.read_bytes(), path.name)
        found = self.call("brief", "example", "--projection", "compact", "--require-current")
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": found["snapshot_id"],
                   "file_paths": ["src/example.cpp", "src/unsafe.cpp"]}
        result = self.read(request)
        self.assertEqual(result["evidence"]["bodies"][0]["text"].encode(), self.body)
        self.assertIsNone(result["evidence"]["bodies"][1]["text"])
        bad.write_text('int recovered() { return 9; }\n')
        document.write_text('# Safe configuration\n\nNo credentials stored here.\n')
        self.call("build")
        doctor = self.call("doctor", "--strict")
        self.assertEqual(doctor["counts"]["quarantined_files"], 0)
        found = self.call("brief", "recovered", "--projection", "compact", "--require-current")
        result = self.read({**request, "snapshot_id": found["snapshot_id"], "file_paths": ["src/unsafe.cpp"]})
        self.assertIn("return 9", result["evidence"]["bodies"][0]["text"])

    def test_forged_quarantine_does_not_hide_published_source_rows(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('int example() { return 3; }\nconst char* token = "' + secret + '";\n')
        value = 14695981039346656037
        for byte in self.source.read_bytes():
            value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        manifest_path = self.out / "docs/kb/generated/manifest.json"
        manifest = json.loads(manifest_path.read_text())
        entry = next(row for row in manifest["source_fact_sources"] if row["path"] == "src/example.cpp")
        entry.update(quarantine_reason="openai-api-key", content_hash=f"fnv1a64:{value:016x}",
                     bytes=self.source.stat().st_size)
        manifest_path.write_text(json.dumps(manifest))
        result = subprocess.run([*self.prefix, "doctor", "--strict"], capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(secret.encode(), result.stdout + result.stderr)
        self.assertIn(b"secret-like-source-content", result.stdout)

    def test_whole_file_v2_malformed_selectors(self):
        request = {"schema_version": "shin.source-read-request.v2", "snapshot_id": self.request["snapshot_id"]}
        for paths in ([], [1], [None], [True], [""], ["src/\u0000example.cpp"], ["src\\example.cpp"],
                      ["src/example.cpp"] * 5):
            self.read({**request, "file_paths": paths}, code=2)
        self.call("read", "--input", "-", input=(json.dumps({**request, "file_paths": ["src/example.cpp"]})[:-1]
                  + ',"file_paths":[]}').encode(), code=2)


if __name__ == "__main__":
    unittest.main()
