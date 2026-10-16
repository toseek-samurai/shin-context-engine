#!/usr/bin/env python3
"""Validate the dev-only port override before Docker or private state mutation."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("shin_dev", Path(__file__).resolve().parents[1] / "tools/dev.py")
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)


class DevPortTests(unittest.TestCase):
    def test_default_port_unchanged(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(dev.postgres_port(), 15432)

    def test_valid_override_used_by_conninfo(self):
        with patch.dict(os.environ, {"SHIN_SERVER_DEV_POSTGRES_PORT": "25432"}):
            self.assertEqual(dev.postgres_port(), 25432)
            self.assertIn("port=25432 ", dev._conninfo(role="fixture", password="fixture"))

    def test_invalid_override_is_refused(self):
        for value in ("", "0", "-1", "65536", "015432", "15432\nhost=external", "abc", " 25432"):
            with self.subTest(value=value), patch.dict(os.environ, {"SHIN_SERVER_DEV_POSTGRES_PORT": value}):
                with self.assertRaises(dev.DevStateError):
                    dev.postgres_port()

    def test_compose_environment_pins_explicit_port_without_rewriting_unknown_state(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {"SHIN_SERVER_DEV_POSTGRES_PORT": "25432"}):
            root = Path(temp)
            dev._ensure_compose_environment(root)
            path = root / "compose.env"
            self.assertIn("SHIN_SERVER_DEV_POSTGRES_PORT=25432\n", path.read_text())
            original = path.read_bytes()
            dev._ensure_compose_environment(root)
            self.assertEqual(path.read_bytes(), original)
            with patch.dict(os.environ, {"SHIN_SERVER_DEV_POSTGRES_PORT": "25433"}):
                with self.assertRaises(dev.DevStateError):
                    dev._ensure_compose_environment(root)
                self.assertEqual(path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
