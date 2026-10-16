#!/usr/bin/env python3
"""Verify allocator scope guards before any dependency download or native build."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AllocatorConfigTests(unittest.TestCase):
    def check_config(self, allocator, platform="Linux", cli="ON", sanitizer="OFF", error=None):
        with tempfile.TemporaryDirectory(prefix="shin-allocator-config-") as directory:
            root = Path(directory)
            script = root / "test.cmake"
            script.write_text(
                f'set(SHIN_CORE_ALLOCATOR "{allocator}" CACHE STRING "")\n'
                f'set(CMAKE_SYSTEM_NAME "{platform}")\n'
                f'set(SHIN_CONTEXT_BUILD_CLI {cli})\n'
                f'set(SHIN_ENABLE_SANITIZERS {sanitizer})\n'
                f'include("{ROOT}/shared/cmake/CoreAllocator.cmake")\n')
            result = subprocess.run(["cmake", "-P", str(script)], cwd=root,
                                    capture_output=True, text=True, timeout=15)
            if error:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
            else:
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((root / "_deps").exists())

    def test_system_allocator_needs_no_dependencies_on_any_lane(self):
        for platform in ("Linux", "Darwin"):
            self.check_config("system", platform, cli="OFF", sanitizer="ON")

    def test_unknown_allocator_is_rejected(self):
        self.check_config("arbitrary", error="SHIN_CORE_ALLOCATOR must be")

    def test_override_is_rejected_for_mac_embedded_core_and_sanitizers(self):
        for kwargs in ({"platform": "Darwin"}, {"cli": "OFF"}, {"sanitizer": "ON"}):
            self.check_config("mimalloc", error="non-sanitized standalone Linux Core CLI only", **kwargs)


if __name__ == "__main__":
    unittest.main()
