#!/usr/bin/env python3
"""Native CLI smoke; no WSL, shell, live projects or installed data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CORE, PROFILES = (Path(sys.argv.pop(1)).resolve() for _ in range(2))


class WindowsCoreSmoke(unittest.TestCase):
    def scenario(self, suffix):
        with tempfile.TemporaryDirectory(prefix="shin-win-core-") as directory:
            base = Path(directory)
            root = base / suffix
            root.mkdir(parents=True)
            index = base / "index"
            source = root / "README.md"
            source.write_text("# Retry Policy\n\n## Retry Ceiling\n\nThe retry ceiling is seven attempts.\n", encoding="utf-8")

            def run(*command, success=True):
                result = subprocess.run([str(CORE), "--profile", "generic-docs", "--profile-dir", str(PROFILES),
                                         "--root", str(root), "--out-dir", str(index), *command],
                                        capture_output=True, encoding="utf-8", timeout=30)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
                return json.loads(result.stdout or result.stderr)

            run("build")
            self.assertTrue(run("check")["current"])
            run("doctor", "--strict")
            first = run("brief", "retry ceiling", "--projection", "compact", "--require-current")
            self.assertTrue(first["ok"])
            self.assertIn("README.md", [row["path"] for row in first["evidence"]])
            source.write_text("# Retry Policy\n\n## Retry Ceiling\n\nThe retry ceiling is nine attempts.\n", encoding="utf-8")
            run("brief", "retry ceiling", "--projection", "compact", "--require-current", success=False)
            run("build")
            second = run("brief", "retry ceiling", "--projection", "compact", "--require-current")
            self.assertNotEqual(first["snapshot_id"], second["snapshot_id"])
            self.assertTrue(second["index_freshness"]["current"])
            self.assertFalse((root / ".git").exists())

    def test_unicode_root(self):
        self.scenario("\u041f\u0440\u043e\u0435\u043a\u0442 \u6e2c\u8a66")

    def test_long_root(self):
        self.scenario(Path(*(["segment-" + "x" * 45] * 6)))


if __name__ == "__main__":
    unittest.main()
