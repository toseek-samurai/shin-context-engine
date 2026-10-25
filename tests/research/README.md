# Retrieval and agent experiments

These modules share preparation, publication validation and policy helpers.
Keeping the import cohort together lets a script run directly without installing
a test package or modifying the user's Python configuration. Repository inputs
are resolved from the source root; policies and synthetic corpora stay under
[`../fixtures`](../fixtures/).

| Area | Entry points and helpers |
| --- | --- |
| Semantic preparation and projection | `prepare_semantic_pilot.py`, `reproject_semantic_*.py`, `semantic_chunk_projection.py`, `materialize_semantic_artifact.py` |
| Offline retrieval comparisons | `run_local_semantic_pilot.py`, `run_project_search_comparison.py`, `run_standard_retrieval.py`, `run_semantic_hybrid.py`, `run_cross_encoder_reference.py`, `run_hierarchical_retrieval.py` |
| Source projection drivers | `semantic_source_export.cpp`, `semantic_lexical_export.cpp`, `semantic_source_hop.py` |
| Agent workflows and paired runs | `run_agent_query_pilot.py`, `code_context_workflow.py`, `code_context_pair.py`, `run_code_context_*.py`, `run_answer_grounding_probe.py` |
| Saved-result analysis | `analyze_*.py`, `check_*.py`, `evaluate_semantic_*.py`, `review_semantic_misses.py` |
| Operator probes | `capture_local_review.py`, `probe_*.py`, `run_query_metrics_pilot.py` |
| Regression tests | `test_*.py`, `test_patch_pilot_guard.mjs`; use the owning CMake registration where present. |

CTest runs the registered hermetic checks. It does not start model downloads,
paid provider calls, agent comparison campaigns or probes of a live installation.
Optional model experiments use the pinned `semantic-pilot-requirements.txt` and
explicitly supplied model assets. Operator probes and agent runs require their
own reviewed inputs and host setup.

## Standalone prototype gaps

The directory move preserves the following existing gaps outside registered
CTest qualification:

- `test_codex_patch_pilot.py` has six failing command-execution checks.
  `codex_patch_guard.execute_admitted` references an undefined `native` variable;
  the CLI reports `pilot-hook-state-invalid`. The failures also reproduce with
  the original files before relocation. Its admission and output assertions
  remain intact.
- `test_bff_patch_admission.py` imports `probe_bff_patch_admission`, which is not
  provided by this public checkout. Its standalone harness is incomplete here.

These scripts require separate qualification before operational use. Their
failures must not be counted as passing tests or hidden by changing assertions.
