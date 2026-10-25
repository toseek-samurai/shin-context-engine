#!/usr/bin/env python3
"""Current Core -> semantic generation -> query and replacement boundary tests."""

import argparse
from copy import deepcopy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import test_semantic_runtime as fixtures
from shin_semantic.artifact import Artifact, Rejected, decode, encode, read, sha
from shin_semantic import producer, runtime
from shin_semantic.projection import capture_publication, project_units

ROOT = Path(__file__).resolve().parents[2]
CORE = EXPORTER = MODEL_DIR = STABLE_CORE = None


class ProducerTests(fixtures.FixtureTests):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("run vector producer tests in the isolated semantic runtime")
        import numpy as np
        self.np = np
        super().setUp()
        source_dir = self.fixture.root / "src"
        source_dir.mkdir()
        self.source = source_dir / "cache.ts"
        self.source.write_text("export function cacheScope(brand: string) {\n  return ['cache', brand];\n}\n")
        profile = decode(self.fixture.profile_path.read_bytes())
        profile.update(sourceFactRoots=["src"], sourceFactExtensions=[".ts"], sourceFactExtractors=["language"])
        fixtures.write(self.fixture.profile_path, encode(profile))
        self.build()
        self.model_dir.mkdir()
        self.config["profile_sha256"] = sha(self.fixture.profile_path.read_bytes())
        self.store = self.base / "semantic-store"
        self.build_config_path = self.base / "build.json"
        self.build_config = {"schema_version": "shin.semantic-build.v1",
            "route": {key: self.config[key] for key in runtime.BASE_ROUTE_FIELDS},
            "exporter_binary": str(EXPORTER), "exporter_sha256": sha(EXPORTER.read_bytes()), "store_dir": str(self.store)}
        self.save_build_config()
        self.inputs = []
        self.on_embed = None
        outer = self

        class Encoder:
            def embed(self, texts, **kwargs):
                values = list(texts)
                outer.inputs.extend(values)
                if outer.on_embed is not None:
                    callback, outer.on_embed = outer.on_embed, None
                    callback()
                vectors = []
                for text in values:
                    vector = np.zeros(384, dtype=np.float32)
                    vector[int(sha(text.encode())[:4], 16) % 384] = 1
                    vectors.append(vector)
                return iter(vectors)

        self.encoder = Encoder()
        self.model_patch = patch.object(producer.model, "load", return_value=self.encoder)
        self.loader = self.model_patch.start()
        self.addCleanup(self.model_patch.stop)

    def save_build_config(self):
        fixtures.write(self.build_config_path, encode(self.build_config))

    def produce(self, reuse=None):
        return producer.Producer(self.build_config_path).build(reuse)

    def active(self):
        path = self.store / "current-route.json"
        config = decode(path.read_bytes())
        return path, config, Artifact(Path(config["artifact_dir"]), config["artifact_sha256"], config)

    def change(self):
        self.source.write_text(self.source.read_text().replace("'cache'", "'changed'") +
                               "export function partitionOwner() { return 42; }\n")
        self.build()

    def test_first_build_and_query_use_current_native_publication_without_mutation(self):
        before = [fixtures.snapshot(p) for p in (self.fixture.root, self.fixture.out, self.fixture.profiles)]
        result = self.produce()
        self.assertTrue(result["ok"])
        self.assertTrue(result["activated"])
        self.assertEqual(result["vectors_computed"], result["units"])
        self.assertEqual(result["vectors_reused"], 0)
        route, _, artifact = self.active()
        reply = runtime.Session(route).query("Describe partitioning rules for saved responses")
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["core"]["snapshot_id"], artifact.manifest["binding"]["snapshot_id"])
        self.assertEqual(before, [fixtures.snapshot(p) for p in (self.fixture.root, self.fixture.out, self.fixture.profiles)])
        self.assertTrue(all(text.startswith("passage: ") for text in self.inputs[:-1]))

    def test_unchanged_generation_reuses_every_vector_without_model_or_route_rewrite(self):
        first = self.produce()
        route, _, _ = self.active()
        before = route.read_bytes(), route.stat().st_mtime_ns
        self.model_dir.rmdir()
        self.loader.side_effect = AssertionError("unchanged input must not load a model")
        again = self.produce()
        self.assertEqual(again["vectors_computed"], 0)
        self.assertEqual(again["vectors_reused"], first["units"])
        self.assertFalse(again["activated"])
        self.assertEqual((route.read_bytes(), route.stat().st_mtime_ns), before)
        self.assertEqual(len(list((self.store / "generations").iterdir())), 1)
        self.assertFalse(list(self.store.glob(".stage-*")))

    def test_changed_input_is_computed_but_other_unit_vectors_are_preserved(self):
        self.produce()
        route, _, old = self.active()
        self.change()
        with self.assertRaisesRegex(Rejected, "semantic-snapshot-mismatch"):
            runtime.Session(route).query("Describe partitioning rules for saved responses")
        result = self.produce()
        self.assertGreater(result["vectors_reused"], 0)
        self.assertGreater(result["vectors_computed"], 0)
        _, _, new = self.active()
        old_rows = {row["text_sha256"]: i for i, row in enumerate(old.units)}
        for i, row in enumerate(new.units):
            if row["text_sha256"] in old_rows:
                self.np.testing.assert_array_equal(new.matrix[i], old.matrix[old_rows[row["text_sha256"]]])
        reply = runtime.Session(route).query("Describe partitioning rules for saved responses")
        self.assertTrue(reply["ok"])

    def test_same_model_text_reuses_vectors_but_refreshes_source_provenance(self):
        self.produce()
        _, _, old = self.active()
        self.source.write_text(self.source.read_text() + "// A comment outside the original function.\n")
        self.build()
        result = self.produce()
        _, _, new = self.active()
        original = next(row for row in old.units if row["path"] == "src/cache.ts")
        updated = next(row for row in new.units if row["text_sha256"] == original["text_sha256"])
        self.assertNotEqual(original["source_sha256"], updated["source_sha256"])
        self.assertEqual(updated["source_sha256"], sha(self.source.read_bytes()))
        self.assertGreater(result["vectors_reused"], 0)

    def test_deleted_source_is_removed_not_resurrected_from_cache(self):
        self.produce()
        self.source.unlink()
        self.build()
        result = self.produce()
        _, _, artifact = self.active()
        self.assertFalse(any(row["path"] == "src/cache.ts" for row in artifact.units))
        self.assertEqual(result["vectors_computed"], 0)

    def test_stale_core_is_rejected_before_model_load(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.source.write_text("export function notPublished() {}\n")
        self.loader.reset_mock()
        with self.assertRaises(Rejected):
            self.produce()
        self.loader.assert_not_called()
        self.assertEqual(route.read_bytes(), before)

    def test_source_change_during_embedding_keeps_previous_route(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.change()
        self.on_embed = lambda: self.source.write_text("export function laterChange() {}\n")
        with self.assertRaises(Rejected):
            self.produce()
        self.assertEqual(route.read_bytes(), before)
        self.assertFalse(list(self.store.glob(".stage-*")))

    def test_concurrent_new_publication_is_not_mixed_into_captured_generation(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.change()
        self.on_embed = self.change
        with self.assertRaisesRegex(Rejected, "publication-changed-during-build"):
            self.produce()
        self.assertEqual(route.read_bytes(), before)

    def test_profile_change_requires_explicit_config_and_disables_reuse(self):
        self.produce()
        profile = decode(self.fixture.profile_path.read_bytes())
        profile["sourceLabel"] = "Changed fixture policy"
        fixtures.write(self.fixture.profile_path, encode(profile))
        self.build()
        with self.assertRaisesRegex(Rejected, "profile-changed"):
            self.produce()
        self.build_config["route"]["profile_sha256"] = sha(self.fixture.profile_path.read_bytes())
        self.save_build_config()
        result = self.produce()
        self.assertEqual(result["vectors_reused"], 0)
        self.assertEqual(result["vectors_computed"], result["units"])

    def test_model_failure_or_bad_vector_never_activates_partial_data(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.change()
        self.loader.side_effect = FileNotFoundError("private diagnostic")
        with self.assertRaises(FileNotFoundError):
            self.produce()
        self.assertEqual(route.read_bytes(), before)
        self.loader.side_effect = None
        with patch.object(self.encoder, "embed", return_value=iter([self.np.zeros(384, dtype="f4")] * 4)):
            with self.assertRaises(Rejected):
                self.produce()
        self.assertEqual(route.read_bytes(), before)

    def test_build_config_or_active_route_changed_during_work_is_not_overwritten(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.change()
        self.on_embed = lambda: fixtures.write(self.build_config_path, self.build_config_path.read_bytes() + b" ")
        with self.assertRaisesRegex(Rejected, "build-config-changed"):
            self.produce()
        self.assertEqual(route.read_bytes(), before)
        self.save_build_config()
        self.on_embed = lambda: fixtures.write(route, before + b" ")
        with self.assertRaisesRegex(Rejected, "active-route-changed"):
            self.produce()
        self.assertEqual(route.read_bytes(), before + b" ")

    def test_two_processes_cannot_write_same_store(self):
        self.produce()
        with producer.writer_lock(self.store):
            result = subprocess.run([sys.executable, str(ROOT / "tools/shin_semantic_build.py"), "--config", str(self.build_config_path)],
                                    capture_output=True, timeout=5)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(decode(result.stdout)["error"]["code"], "semantic-writer-busy")

    def test_failure_before_pointer_swap_keeps_old_and_complete_orphan_is_reusable(self):
        self.produce()
        route, _, _ = self.active()
        before = route.read_bytes()
        self.change()
        with patch.object(producer.os, "replace", side_effect=OSError("injected-before-commit")):
            with self.assertRaises(OSError):
                self.produce()
        self.assertEqual(route.read_bytes(), before)
        result = self.produce()
        self.assertTrue(result["ok"])
        self.assertTrue(result["activated"])
        self.active()

    def test_failure_after_pointer_swap_reports_complete_but_durability_uncertain(self):
        real = producer.sync_directory
        def fail_store(path):
            if path == self.store:
                raise OSError("injected-after-commit")
            real(path)
        with patch.object(producer, "sync_directory", side_effect=fail_store):
            result = self.produce()
        self.assertFalse(result["ok"])
        self.assertTrue(result["activated"])
        self.assertEqual(result["error"]["code"], "publication-durability-uncertain")
        self.active()

    def test_store_cannot_write_into_inputs(self):
        for path in (self.fixture.root, self.fixture.out, self.fixture.profiles, self.model_dir):
            with self.subTest(path=path):
                self.build_config["store_dir"] = str(path)
                self.save_build_config()
                before = fixtures.snapshot(path)
                with self.assertRaises(Rejected):
                    self.produce()
                self.assertEqual(fixtures.snapshot(path), before)

    def test_corrupt_donor_is_not_silently_ignored(self):
        self.produce()
        route, config, _ = self.active()
        before = route.read_bytes()
        path = Path(config["artifact_dir"]) / "vectors.npy"
        fixtures.write(path, b"corrupt")
        with self.assertRaises(Rejected):
            self.produce()
        self.assertEqual(route.read_bytes(), before)

    def test_explicit_donor_is_first_build_only_and_scope_bound(self):
        self.produce()
        route, config, _ = self.active()
        self.build_config["store_dir"] = str(self.base / "other-store")
        self.save_build_config()
        result = self.produce(route)
        self.assertEqual(result["vectors_computed"], 0)
        with self.assertRaisesRegex(Rejected, "explicit-reuse-only-on-initial-build"):
            self.produce(route)
        self.build_config["store_dir"] = str(self.base / "foreign-store")
        self.build_config["route"]["project"] = "foreign"
        self.save_build_config()
        with self.assertRaisesRegex(Rejected, "donor-scope-mismatch"):
            self.produce(route)

    def test_native_redaction_precedes_model_input_and_export_is_not_persisted(self):
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('export function credentialExample() { return "' + secret + '"; } // gitleaks:allow\n')
        self.build()
        self.produce()
        self.assertFalse(any(secret in text for text in self.inputs))
        self.assertTrue(any("[REDACTED:" in text for text in self.inputs))
        self.assertFalse(list(self.store.rglob("source-ranges.jsonl")))

    @unittest.expectedFailure
    def test_core_suppression_policy_observation(self):
        # Existing Core suppression policy is separate from the stricter producer.
        # Retain the failing expectation until that compatibility decision is made.
        secret = "sk-" + "Q7x9" * 16
        self.source.write_text('export function credentialExample() { return "' + secret + '"; } // gitleaks:allow\n')
        for number, binary in enumerate([CORE, *([STABLE_CORE] if STABLE_CORE else [])]):
            with self.subTest(binary=str(binary)):
                index = self.base / ("brief-control-" + str(number))
                prefix = [str(binary), "--root", str(self.fixture.root), "--profile", "observer-fixture", "--profile-dir",
                          str(self.fixture.profiles), "--out-dir", str(index)]
                build = subprocess.run([*prefix, "build"], capture_output=True, timeout=10)
                self.assertEqual(build.returncode, 0)
                query = subprocess.run([*prefix, "--from-index", "brief", "credentialExample", "--projection", "compact", "--require-current"],
                                       capture_output=True, timeout=10)
                self.assertEqual(query.returncode, 0)
                self.assertFalse(secret.encode() in query.stdout)

    def test_full_document_span_is_scanned_despite_source_suppression(self):
        secret = "sk-" + "Z8w2" * 16
        self.fixture.write_source('# Example\n\nconst api_key = "' + secret + '"; // gitleaks:allow\n')
        self.build()
        result = self.produce()
        self.assertGreater(result["projection"]["fully_redacted_units"], 0)
        self.assertFalse(any(secret[:8] in text for text in self.inputs))

    def test_secret_after_summary_clipping_redacts_complete_model_input(self):
        secret = "sk-" + "Z8w2" * 16
        self.source.write_text('export function longExample() { return "' + "x" * 2030 +
                               " " + secret + '"; } // gitleaks:allow\n')
        self.build()
        result = self.produce()
        self.assertGreater(result["projection"]["fully_redacted_units"], 0)
        self.assertFalse(any(secret[:8] in text for text in self.inputs))

    def test_projection_rejects_source_changed_between_export_and_metadata_read(self):
        instance = producer.Producer(self.build_config_path)
        publication, _, files = capture_publication(instance.route)
        output = io.BytesIO()
        producer.capture_command([str(EXPORTER), str(self.fixture.root), str(self.fixture.out)], output, 1024 * 1024, 10)
        self.source.write_text(self.source.read_text() + "// changed after export\n")
        with self.assertRaisesRegex(Rejected, "source-changed-during-projection"):
            project_units(instance.route, publication, files, output.getvalue())

    def test_real_offline_build_repeat_and_query(self):
        if MODEL_DIR is None:
            self.skipTest("supply --model-dir for real offline inference")
        self.model_patch.stop()
        self.build_config["route"]["model_dir"] = str(MODEL_DIR)
        self.save_build_config()
        args = [sys.executable, str(ROOT / "tools/shin_semantic_build.py"), "--config", str(self.build_config_path)]
        first = subprocess.run(args, capture_output=True, timeout=60)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual(first.stderr, b"")
        result = decode(first.stdout)
        self.assertGreater(result["vectors_computed"], 0)
        second = subprocess.run(args, capture_output=True, timeout=60)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(decode(second.stdout)["vectors_computed"], 0)
        query = subprocess.run([sys.executable, str(ROOT / "tools/shin_semantic.py"), "--config", result["route"],
            "Describe partitioning rules for saved responses"], capture_output=True, timeout=30)
        self.assertEqual(query.returncode, 0, query.stdout + query.stderr)
        self.assertEqual(query.stderr, b"")
        self.assertTrue(decode(query.stdout)["ok"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=Path)
    parser.add_argument("--exporter", required=True, type=Path)
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--stable-core", type=Path)
    parser.add_argument("--report", type=Path)
    args, remaining = parser.parse_known_args()
    CORE, EXPORTER = args.core.resolve(), args.exporter.resolve()
    MODEL_DIR = args.model_dir.resolve() if args.model_dir else None
    STABLE_CORE = args.stable_core.resolve() if args.stable_core else None
    fixtures.CORE = CORE
    program = unittest.main(argv=[sys.argv[0], *remaining], exit=False)
    if args.report:
        paths = [ROOT / "integrations/semantic/producer.py", ROOT / "integrations/semantic/projection.py",
                 ROOT / "tools/shin_semantic_build.py", ROOT / "tools/semantic_source_export.cpp", Path(__file__)]
        result = {"schema_version": "shin.semantic-producer-tests.v1", "ok": program.result.wasSuccessful(),
            "tests": program.result.testsRun, "skipped": len(program.result.skipped), "failures": len(program.result.failures),
            "errors": len(program.result.errors), "real_model_requested": MODEL_DIR is not None,
            "expected_core_policy_failures": len(program.result.expectedFailures),
            "stable_compact_control_requested": STABLE_CORE is not None,
            "core_sha256": sha(CORE.read_bytes()), "exporter_sha256": sha(EXPORTER.read_bytes()),
            "implementation_sha256": {str(path.relative_to(ROOT)): sha(path.read_bytes()) for path in paths}}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_bytes(encode(result) + b"\n")
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
