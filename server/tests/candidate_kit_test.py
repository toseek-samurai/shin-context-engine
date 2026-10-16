#!/usr/bin/env python3
"""Candidate packaging guards; native template behavior is checked by assembly."""

import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("candidate_kit", Path(__file__).parents[1] / "tools/candidate_kit.py")
kit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(kit)


class CandidateKitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prefix = self.root / "prefix"
        self.prefix.mkdir()
        self.file = self.prefix / "payload"
        self.file.write_text("original")

    def test_inventory_detects_content_mode_missing_and_extra_files(self):
        manifest = {"files": kit.inventory(self.prefix)}
        kit.verify(self.prefix, manifest)
        self.file.write_text("modified")
        with self.assertRaises(ValueError):
            kit.verify(self.prefix, manifest)
        self.file.write_text("original")
        mode = self.file.stat().st_mode & 0o777
        self.file.chmod(mode ^ 0o100)
        with self.assertRaises(ValueError):
            kit.verify(self.prefix, manifest)
        self.file.chmod(mode)
        extra = self.prefix / "extra"
        extra.touch()
        with self.assertRaises(ValueError):
            kit.verify(self.prefix, manifest)
        extra.unlink()
        self.file.unlink()
        with self.assertRaises(ValueError):
            kit.verify(self.prefix, manifest)

    def test_symlinks_rejected_including_manifest(self):
        for name in ("link", kit.MANIFEST):
            with self.subTest(name=name):
                path = self.prefix / name
                path.symlink_to(self.file)
                with self.assertRaises(ValueError):
                    kit.inventory(self.prefix)
                path.unlink()

    def test_manifest_is_not_self_hashed(self):
        rows = kit.inventory(self.prefix)
        kit.write_json(self.prefix / kit.MANIFEST, {"files": rows})
        kit.verify(self.prefix, kit.read_json(self.prefix / kit.MANIFEST))

    def test_allocator_policy_requires_exact_pin_linux_and_license(self):
        self.assertIsNone(kit.core_allocator(self.prefix))
        policy = self.prefix / "share/shin-context/shin-core-allocator.json"
        policy.parent.mkdir(parents=True)
        value = {"schema_version": "shin.core-allocator.v1", "id": "mimalloc", "version": "3.5.3",
                 "scope": "standalone-linux-core-cli", "linkage": "static-object",
                 "transparent_huge_pages": False,
                 "source_sha256": "3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47"}
        kit.write_json(policy, value)
        with patch.object(kit.platform, "system", return_value="Linux"):
            with self.assertRaises(FileNotFoundError):
                kit.core_allocator(self.prefix)
            license = policy.parent / "licenses/mimalloc/LICENSE"
            license.parent.mkdir(parents=True)
            license.write_text("MIT fixture")
            self.assertEqual(kit.core_allocator(self.prefix), value)
            for key, changed in (("version", "3.5.2"), ("transparent_huge_pages", True),
                                 ("transparent_huge_pages", 0),
                                 ("linkage", "preload"), ("source_sha256", "unknown")):
                kit.write_json(policy, {**value, key: changed})
                with self.assertRaises(ValueError):
                    kit.core_allocator(self.prefix)
            kit.write_json(policy, value)
        with patch.object(kit.platform, "system", return_value="Darwin"), self.assertRaises(ValueError):
            kit.core_allocator(self.prefix)

    def test_dependency_sdk_is_rejected(self):
        self.file.unlink()
        for name in ("bin", "share"):
            (self.prefix / name).mkdir()
        kit.runtime_layout(self.prefix)
        (self.prefix / "lib").mkdir()
        with self.assertRaises(ValueError):
            kit.runtime_layout(self.prefix)

    def test_linux_libraries_keep_first_line_and_missing_library_fails(self):
        self.assertEqual(kit.library_lines("first\nsecond\n", "Linux"), ["first", "second"])
        self.assertEqual(kit.library_lines("binary:\nlib\n", "Darwin"), ["lib"])
        with self.assertRaises(ValueError):
            kit.library_lines("libpq.so => not found", "Linux")

    def test_structured_template_expansion_preserves_types(self):
        value = {"root": "${ROOT}/source", "list": [False, 42, "${PREFIX}/profiles"]}
        self.assertEqual(kit.expand(value, {"ROOT": "/a", "PREFIX": "/b"}),
                         {"root": "/a/source", "list": [False, 42, "/b/profiles"]})

    def test_existing_output_never_overwritten(self):
        output = self.root / "build/existing"
        output.mkdir(parents=True)
        marker = output / "preserve"
        marker.write_text("keep")
        with patch.object(kit, "ROOT", self.root), self.assertRaises(FileExistsError):
            kit.assemble(SimpleNamespace(output=output))
        self.assertEqual(marker.read_text(), "keep")

    def test_outside_output_rejected(self):
        output = self.root / "outside"
        with patch.object(kit, "ROOT", self.root), self.assertRaises(ValueError):
            kit.assemble(SimpleNamespace(output=output))
        self.assertFalse(output.exists())

    def test_failed_identity_cleans_owned_temporary_stage(self):
        build = self.root / "build/reference"
        build.mkdir(parents=True)
        args = SimpleNamespace(output=self.root / "build/output", core_build=build,
                               server_build=build, observer_build=build)
        with patch.object(kit, "ROOT", self.root), patch.object(kit, "run", return_value=json.dumps({"ok": False})):
            with self.assertRaises(ValueError):
                kit.assemble(args)
        self.assertEqual(list(args.output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
