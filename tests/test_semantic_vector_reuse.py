#!/usr/bin/env python3
"""Matrix reuse integration fixtures; run in the pinned pilot venv, without inference."""

import importlib.metadata
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from run_carry_forward_comparison import save
from run_local_semantic_pilot import copy_reusable_vectors, fingerprint, vector_input_key


class VectorReuseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.donor = self.base / "donor"
        self.source = self.donor / "input"
        self.source.mkdir(parents=True)
        self.model = self.base / "model"
        self.model.mkdir()
        save(self.model / "manifest.json", {"fixture": True})
        self.policy = {"projection": "body", "dimensions": 2, "threads": 4, "batch_size": 4,
                       "max_units": 10, "max_input_bytes": 10000}
        self.units = [{"project": "p", "path": "source.cpp", "start_line": i + 1, "end_line": i + 1,
                       "source_sha256": "source", "record_ids": [str(i)], "text": str(i)} for i in range(2)]
        (self.source / "units.jsonl").write_text("".join(json.dumps(u) + "\n" for u in self.units))
        save(self.source / "policy.json", self.policy)
        save(self.source / "spec.json", {"fixture": True})
        self.manifest = {"units": 2, "projects": [{"id": "p", "root": "/fixture", "profile_sha256": "profile",
                         "sources": {"source.cpp": {"sha256": "source"}}}], **{
            key: fingerprint(self.source / name) for name, key in (("units.jsonl", "units_sha256"),
                ("spec.json", "spec_sha256"), ("policy.json", "policy_sha256"))}}
        save(self.source / "input-manifest.json", self.manifest)
        self.binding = {"input": str(self.source), "input_manifest_sha256": fingerprint(self.source / "input-manifest.json"),
                        "policy": self.policy}
        save(self.donor / "run-binding.json", self.binding)
        self.values = np.eye(2, dtype=np.float32)
        np.save(self.donor / "vectors.npy", self.values)
        self.report = {"completed": True, "units": 2, "vectors_sha256": fingerprint(self.donor / "vectors.npy"),
                      "model_manifest_sha256": fingerprint(self.model / "manifest.json"),
                      "runtime_versions": {n: importlib.metadata.version(n) for n in ("fastembed", "onnxruntime", "tokenizers", "numpy")},
                      "runtime_options": {"enable_cpu_mem_arena": True, "providers": ["CPUExecutionProvider"],
                          "threads": 4, "batch_size": 4, "input_loading": "streamed-jsonl"}}
        save(self.donor / "report.json", self.report)

    def copy(self, units=None, mode="augmented", manifest=None, policy=None, admission=None):
        units = self.units if units is None else units
        self.destination = np.full((len(units), 2), np.nan, dtype=np.float32)
        return copy_reusable_vectors(self.donor, manifest or self.manifest, policy or self.policy,
                                     units, self.model, self.destination, mode, admission)

    def test_copy_preserves_destination_order_and_leaves_new_rows_unwritten(self):
        changed, receipt = self.copy([self.units[1], {**self.units[0], "text": "changed"}, self.units[0]])
        self.assertEqual(changed, [1])
        np.testing.assert_array_equal(self.destination[[0, 2]], self.values[[1, 0]])
        self.assertTrue(np.isnan(self.destination[1]).all())
        self.assertEqual((receipt["copied_vectors"], receipt["recomputed_vectors"], receipt["unused_donor_vectors"]), (2, 1, 0))
        self.assertEqual(self.copy([self.units[1]])[1]["unused_donor_vectors"], 1)

    def test_exact_mode_remains_strict_and_allows_only_changed_text(self):
        self.assertEqual(self.copy(mode="exact")[0], [])
        self.assertEqual(self.copy([self.units[0], {**self.units[1], "text": "new"}], "exact")[0], [1])
        for units in ([self.units[1], self.units[0]], [self.units[1]]):
            with self.assertRaises(ValueError):
                self.copy(units, "exact")

    def test_profile_rebound_copies_bitwise_and_never_changes_other_modes(self):
        manifest = json.loads(json.dumps(self.manifest))
        manifest["projects"][0]["profile_sha256"] = "new-profile"
        admission = {"schema_version": "shin.semantic-profile-transition.v1",
            "donor_manifest_digest": vector_input_key(self.manifest).hex(),
            "destination_manifest_digest": vector_input_key(manifest).hex(),
            "changes": [{"project": "p", "root": "/fixture", "from": "profile", "to": "new-profile"}]}
        units = [self.units[1], {**self.units[0], "text": "changed"}]
        changed, receipt = self.copy(units, "profile-rebound", manifest, admission=admission)
        self.assertEqual(changed, [1])
        np.testing.assert_array_equal(self.destination[0], self.values[1])
        self.assertTrue(np.isnan(self.destination[1]).all())
        self.assertEqual(receipt["profile_transition"], admission)
        for mode, provided in (("augmented", None), ("exact", None), ("profile-rebound", None), ("augmented", admission)):
            with self.assertRaises(ValueError):
                self.copy(units, mode, manifest, admission=provided)

    def test_hash_bound_but_invalid_donor_values_are_rejected(self):
        for value in (float("nan"), float("inf"), 2.0):
            values = self.values.copy()
            values[0, 0] = value
            np.save(self.donor / "vectors.npy", values)
            save(self.donor / "report.json", {**self.report, "vectors_sha256": fingerprint(self.donor / "vectors.npy")})
            with self.assertRaisesRegex(ValueError, "Invalid donor vector"):
                self.copy()

    def test_partial_failed_runtime_model_and_count_mismatch_are_rejected(self):
        for key, value in (("completed", False), ("units", 1), ("runtime_versions", {}),
                           ("runtime_options", {}), ("model_manifest_sha256", "other")):
            save(self.donor / "report.json", {**self.report, key: value})
            with self.assertRaises(ValueError):
                self.copy()
        (self.donor / "report.json").unlink()
        with self.assertRaises(FileNotFoundError):
            self.copy()

    def test_tampered_matrix_and_wrong_dimensions_or_dtype_are_rejected(self):
        for values in (np.ones((2, 2), dtype=np.float32), np.ones((2, 3), dtype=np.float32),
                       np.ones((2, 2), dtype=np.float64)):
            np.save(self.donor / "vectors.npy", values)
            if values.shape != (2, 2) or values.dtype != np.float32:
                save(self.donor / "report.json", {**self.report, "vectors_sha256": fingerprint(self.donor / "vectors.npy")})
            with self.assertRaises(ValueError):
                self.copy()

    def test_input_binding_and_policy_cannot_be_substituted(self):
        for key, value in (("input_manifest_sha256", "other"), ("policy", {})):
            save(self.donor / "run-binding.json", {**self.binding, key: value})
            with self.assertRaises(ValueError):
                self.copy()
        save(self.donor / "run-binding.json", self.binding)
        with self.assertRaises(ValueError):
            self.copy(policy={**self.policy, "threads": 1})
        with self.assertRaises(ValueError):
            self.copy(manifest={**self.manifest, "spec_sha256": "changed"})
        (self.source / "units.jsonl").write_text("{}\n")
        with self.assertRaises(ValueError):
            self.copy()


if __name__ == "__main__":
    unittest.main()
