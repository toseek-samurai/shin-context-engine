#!/usr/bin/env python3
"""Native process restart/cache proof with isolated homes and non-Git sources."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CORE, DISCOVERY, PROFILES = (Path(sys.argv.pop(1)).resolve() for _ in range(3))


class WindowsSourceCache(unittest.TestCase):
    def scenario(self, suffix):
        with tempfile.TemporaryDirectory(prefix="shin-win-discovery-") as directory:
            base = Path(directory)
            root, index, home = base / suffix, base / "index", base / "home"
            root.mkdir(parents=True)
            home.mkdir()
            source = root / "source.cpp"
            source.write_text("int persistentNeedle() { return 7; }\n", encoding="utf-8")
            queries = base / "queries.json"
            queries.write_text(json.dumps(["persistentNeedle"]), encoding="utf-8")
            env = dict(os.environ, USERPROFILE=str(home))
            env.pop("HOME", None)

            def run(arguments, success=True):
                result = subprocess.run([str(arg) for arg in arguments], env=env, capture_output=True,
                                        encoding="utf-8", timeout=30)
                self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
                return json.loads(result.stdout or result.stderr)

            def build():
                run([CORE, "--profile", "generic-source", "--profile-dir", PROFILES,
                     "--root", root, "--out-dir", index, "build"])

            command = [DISCOVERY, root, index, PROFILES, "generic-source", queries, "--persist"]
            build()
            cold, warm = run(command), run(command)
            self.assertEqual(cold["cache"]["status"], "created")
            self.assertEqual(warm["cache"]["status"], "restored")
            self.assertEqual(warm["reuse"]["index_builds"], 0)
            for result in (cold, warm):
                self.assertGreaterEqual(result["results"][0]["query_ms"], 0)
            self.assertEqual(cold["results"][0]["candidates"], warm["results"][0]["candidates"])
            self.assertEqual(warm["results"][0]["candidates"][0]["path"], "source.cpp")
            files = list((home / ".shin-source-cache").glob("*.sqlite"))
            self.assertEqual(len(files), 1)
            with files[0].open("rb") as database:
                self.assertEqual(database.read(16), b"SQLite format 3\0")
            source.write_text("int changedNeedle() { return 9; }\n", encoding="utf-8")
            self.assertFalse(run(command, success=False)["ok"])
            build()
            queries.write_text(json.dumps(["changedNeedle"]), encoding="utf-8")
            fresh = run(command)
            self.assertEqual(fresh["cache"]["status"], "rebuilt")
            self.assertEqual(fresh["results"][0]["candidates"][0]["path"], "source.cpp")
            self.assertFalse((root / ".git").exists())

    def test_unicode_root(self):
        self.scenario("\u041f\u0440\u043e\u0435\u043a\u0442 \u6e2c\u8a66")

    def test_long_root(self):
        self.scenario(Path(*(["segment-" + "x" * 45] * 6)))


if __name__ == "__main__":
    unittest.main()
