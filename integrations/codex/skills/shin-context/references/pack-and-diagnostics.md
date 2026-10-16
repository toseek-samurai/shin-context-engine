# Pack And Diagnostics

Read for multiple evidence requirements, a reproducible miss, failed quality gates, or an explicit performance investigation. Do not turn a simple Brief into a release audit. Reuse the resolved route from the main skill.

## Context Modes

When the first query group explicitly names one unique path, basename, or
document stem, Brief may report `retrieval_mode=owner-scoped`. This places the
named owner before proxy documents that only mention it. Identity preserves
path components, separators, and explicit extensions; ambiguity, collisions,
or no evidence of an allowed type fall through to ordinary retrieval.

When a profile enables `compoundQueryFirstTermScope`, an extensionless explicit
directory path as the first query term may report `retrieval_mode=directory-scoped`.
Every returned path must be a descendant at a complete path-component boundary;
`Scope2` is outside `Scope`. This explicit branch fails closed when it has no
matching evidence, so do not weaken the path or remove it merely to obtain hits.

Identifier search treats camelCase, PascalCase, snake_case, kebab-case, and
space-separated forms as lexical equivalents. Internal source references are
bounded lexical evidence, not proof of symbol resolution. For compact Brief,
open the exact ranges in `verification_paths` before treating the result as
implementation authority.

Read the current `lexical-retrieval-v1.json` in the resolved Core contract directory for matching and admission. Do not infer prefix matching from query length or copy scorer internals into this workflow.

## Build One Pack

For an implementation task with several explicit evidence needs, place a bounded
`shin.task-request.v1` document under the Shin `build` tree, not in the consumer:

```json
{
  "schema_version": "shin.task-request.v1",
  "request_id": "task-id",
  "task": {
    "objective": "Concrete implementation objective",
    "scope": [{"id": "subsystem", "query": "relevant subsystem contract"}],
    "must": [{"id": "required-evidence", "query": "required source and tests", "require_full_coverage": true}],
    "should": [{"id": "optional-context", "query": "related operational guidance"}]
  },
  "budget": {
    "max_evidence_bytes": 32768,
    "max_records": 24,
    "per_query_records": 8,
    "per_document_records": 2
  }
}
```

Then request one deduplicated Agent Context Pack:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> pack --input <task-request.json> --projection compact --require-current --strict
```

Treat `scope` as an independent evidence query, not a path filter. Confirm
`completion.complete` and `completion.coverage_complete`, inspect each requirement's
`selected_query_coverage` and retrieval `query_coverage`, and use
`verification_paths` to open authoritative source ranges. The receipt is a
deterministic local replay identity, not cryptographic authentication.
Without an explicit coverage policy, one selected record satisfies a requirement
even when normalized groups remain uncovered. Use `require_full_coverage` for all
groups or `min_covered_groups` for a reviewed minimum; never infer missing facts
from default any-evidence completion. Overlapping typed and section representations
may be returned once with all requirement match edges preserved.
`per_document_records` is one Pack-wide ceiling per path across evidence types
and requirements; contracts, facts, and sections from the same file cannot each
consume a separate allowance. Candidate discovery retains up to
`per_query_records` before that global diversity choice. When a later required
query is blocked by a full document slot, the selector may replace an earlier
record only if all already-seeded required coverage remains satisfied.
`per_document_budget_constrained` reports this diversity policy and is not by
itself byte/record saturation.
Use the default full projection only when debugging successful retrieval details.
Add `--telemetry` only for an explicit performance investigation because timing
and RSS fields intentionally make that one result observational rather than
byte-deterministic.
Inspect `budget.pressure` before accepting a nearly saturated Pack. `warning` or
`critical` is advisory rather than a quality failure, but low byte/record
headroom and `top_records`/`top_documents` identify where a reviewed request
should be narrowed instead of blindly increasing limits.

For a failed quality suite, prefer `eval --failed-only` or
`pack-eval --failed-only`. These projections retain whole failed cases, suite
reason codes, omission counts, and safe non-executing diagnose argv while
bounding agent payload size. If a complete case cannot fit, `detail_level=header`
still preserves its diagnostic id, case id, reasons, and primary path; use the
reported detail command or omit `--failed-only` for the complete suite.
First distinguish the owner-declared working, historical and rank-advisory
suites. Run only the suite required for the task; preserve optional misses in
their own reports rather than relabelling them as passed or weakening `must`
assertions. Scope changes require an explicitly authorized owner review.

When several independent local queries must observe exactly one publication,
submit a bounded `shin.batch-request.v1` document through `batch --input` instead
of launching unrelated processes. Require the parent and every child
`snapshot_id` to match.

When a profile-owned path should have appeared but did not, use:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> diagnose '<task query>' --expected-path <relative/path> --require-current
```

Inspect `expectation.reason_codes`, sparse `indexed_rows`, `matching`, and the
shared path rank before changing source documentation, profile policy, or Core.
Use `--strict` only when the expected path is an owned gate.

When reporting a retrieval gap to Shin maintainers, capture a
`shin.retrieval-observation.v1` artifact as defined by
`docs/machine/retrieval-observation-v1.json`. Include exact tool and generator
versions, argv array, profile, root, index mode, snapshot or null source-scan
identity, freshness, query, retrieval mode, coverage, returned paths, exact
verification ranges, and consumer-owned expected/forbidden paths. Product nouns
remain in the profile or observation corpus; a report does not authorize a
consumer-specific Core rule.

For generated-index results, require `snapshot_id` to be non-null and inspect
`index_freshness` before trusting the pack. Compact `brief` exposes only uncovered
query groups; full `brief` and Pack expose `query_coverage.groups` for per-group
inspection. Inspect each full hit's `matched_group_indices` when a document-level
planner supplied broader coverage.
An uncovered group is a retrieval gap, not permission to infer the missing fact.
Source-scan results intentionally use `snapshot_id: null`.
