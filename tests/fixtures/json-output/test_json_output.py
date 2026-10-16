import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(sys.argv.pop(1)).resolve()


class JsonOutputTests(unittest.TestCase):
    def test_profile_label_round_trips_every_ascii_byte_and_unicode(self):
        labels = ["prefix" + chr(value) + "suffix" for value in range(128)]
        labels += ["plain ASCII " * 20, "\u0080", "\u00e9", "\u6f22", "\U0001f642"]
        with tempfile.TemporaryDirectory(prefix="shin-json-output-") as directory:
            base = Path(directory)
            root, profiles, output = base / "root", base / "profiles", base / "index"
            root.mkdir()
            profiles.mkdir()
            (root / "README.md").write_text("# Encoding Fixture\n", encoding="utf-8")
            profile = {
                "schemaVersion": "shin.profile.v1",
                "document": "json-output-fixture",
                "status": "active",
                "id": "json-output",
                "rootFiles": ["README.md"],
                "sourceExtensions": [".md"],
            }
            for label in labels:
                with self.subTest(label=repr(label)):
                    profile["sourceLabel"] = label
                    (profiles / "json-output.json").write_text(json.dumps(profile), encoding="utf-8")
                    result = subprocess.run(
                        [str(TOOL), "--profile", "json-output", "--profile-dir", str(profiles),
                         "--root", str(root), "--out-dir", str(output), "build"],
                        capture_output=True, text=True, timeout=30, check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    manifest = json.loads((output / "docs/kb/generated/manifest.json").read_text(encoding="utf-8"))
                    self.assertEqual(manifest["source"], label)


if __name__ == "__main__":
    unittest.main()
