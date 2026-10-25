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

## Finite usefulness assessments

Keep an assessment separate from model-comparison campaigns and production
configuration. Reuse `source_record` and `metadata_path` from
`run_project_search_comparison.py`, the consumer-owned expectations in
`../fixtures/carry-forward-reference-cases.json`, and, for Brief-only routes,
`../../tools/consumer_golden_replay.py`.

- Freeze query bytes, expected paths, profile, binary hashes and a source manifest
  before observing results. Keep output external. Exclude evaluation metadata
  before indexing, including reference cases, profiles, generated KB and fixture
  answer material. Never change ranking or labels to pass the measured cohort.
- Score native query SOURCE locators and returned bodies separately. Validate
  coordinates against frozen source, report the actual snapshot, selected-source
  freshness, cold/warm state, output bytes and elapsed time. Expected paths are
  reference locations, not an exhaustive list of useful answers. A contract hit
  is distinct from locating its implementation.
- Describe a literal or `rg` baseline precisely: corpus, terms, ordering, limits
  and returned material. A path-list baseline and bounded code context are
  different outputs. Do not infer token savings, task completion or universally
  faster navigation from either output size or process timings.
- Existing-session observations use read-only history and explicit host markers.
  Record tail/retention gaps, probe exclusions and unknown identities. Calls
  embedded in host JavaScript are candidates until cross-checked with durable
  service observations. A gap after compaction can include idle time or another
  task; it is not a measured recovery delay. Do not invent call quotas or model
  attribution from those observations.
- Exercise installation in a disposable prefix and data home. Check handoff
  across adapter restart, task/identity refusals, changed-source review and one
  current-code query without notes. Preserve source, profiles, existing settings
  and histories. A staged source kit is not a qualified published release.
- Report mixed or inconclusive results explicitly. Registered CTest success does
  not turn a low reference-path hit rate into a passing quality assessment.
  Correctness and refusal coverage, retrieval relevance and observed model
  behavior have separate proof boundaries.

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
