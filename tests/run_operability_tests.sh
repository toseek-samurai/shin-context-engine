#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_operability_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
fixture="$source_root/tests/fixtures/basic"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-operability.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
root_two="$work_root/root-two"
out="$work_root/out"
out_two="$work_root/out-two"
profiles="$work_root/profiles"
worktree_root="$work_root/worktree-root"
worktree_out="$work_root/worktree-out"
requirement_root="$work_root/requirement-root"
requirement_out="$work_root/requirement-out"
cp -R "$fixture/root" "$root"
cp -R "$fixture/root" "$root_two"
cp -R "$fixture/root" "$worktree_root"
cp -R "$fixture/root" "$requirement_root"
cp -R "$fixture/profiles" "$profiles"

cat >"$requirement_root/docs/priority-alpha.md" <<'MARKDOWN'
# Alpha Contract

The alpha contract is the primary source for alpha behavior. Beta namespace
enrichment is related context, but it is not authoritative here.
MARKDOWN
cat >"$requirement_root/docs/priority-z-alpha-secondary.md" <<'MARKDOWN'
# Alpha Contract Notes

The alpha contract also has secondary operational notes.
MARKDOWN
cat >"$requirement_root/docs/priority-beta.md" <<'MARKDOWN'
# Beta Namespace Enrichment

The beta namespace enrichment contract is authoritative in this document.
MARKDOWN

shin() {
  "$tool" --profile fixture --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$out" "$@"
}

shin_for() {
  local profile="$1"
  local command_root="$2"
  local command_out="$3"
  shift 3
  "$tool" --profile "$profile" --profile-dir "$profiles" \
    --root "$command_root" --out-dir "$command_out" "$@"
}

shin_pack_diff() {
  local command_root="$1"
  shift
  "$tool" --profile fixture --profile-dir "$fixture/profiles" \
    --root "$command_root" pack-diff "$@"
}

expect_failure() {
  local output="$1"
  shift
  if "$@" >"$output" 2>&1; then
    echo "command unexpectedly succeeded: $*" >&2
    cat "$output" >&2
    exit 1
  fi
}

assert_contains() {
  local file="$1"
  local expected="$2"
  if ! grep -Fq "$expected" "$file"; then
    echo "expected '$expected' in $file" >&2
    cat "$file" >&2
    exit 1
  fi
}

assert_json() {
  local file="$1"
  local expression="$2"
  if ! "$jq_bin" -e "$expression" "$file" >/dev/null; then
    echo "jq assertion failed for $file: $expression" >&2
    cat "$file" >&2
    exit 1
  fi
}

shin build >"$work_root/build.json"
shin_for fixture "$root_two" "$out_two" build >"$work_root/build-two.json"
shin_for fixture "$worktree_root" "$worktree_out" build >"$work_root/worktree-build.json"
shin_for fixture "$requirement_root" "$requirement_out" build \
  >"$work_root/requirement-build.json"

shin_for fixture "$worktree_root" "$worktree_out" worktree-diff --strict \
  >"$work_root/worktree-current.json"
shin_for fixture "$worktree_root" "$worktree_out" worktree-diff --strict \
  >"$work_root/worktree-current-repeat.json"
diff "$work_root/worktree-current.json" "$work_root/worktree-current-repeat.json"
assert_json "$work_root/worktree-current.json" \
  '.schema_version == "shin.worktree-diff.v1" and .ok and .current and
   .comparison.snapshot_integrity == "verified" and
   .comparison.git_required == false and .comparison.read_only and
   .summary.total_changes == 0 and .details_returned == 0 and
   (.snapshot.snapshot_id | startswith("fnv1a64:")) and
   (.worktree.worktree_id | startswith("fnv1a64:"))'

"$jq_bin" '.sourceLabel = "Drifted fixture docs"' "$profiles/fixture.json" \
  >"$work_root/fixture-profile-drift.json"
mv "$work_root/fixture-profile-drift.json" "$profiles/fixture.json"
shin_for fixture "$worktree_root" "$worktree_out" worktree-diff \
  >"$work_root/worktree-profile-drift.json"
assert_json "$work_root/worktree-profile-drift.json" \
  '(.ok | not) and (.current | not) and
   .comparison.generator_current and (.comparison.profile_config_current | not) and
   .summary.metadata_changed == 1 and .summary.total_changes == 1 and
   .details[0].category == "metadata" and .details[0].key == "profile_config_hash"'
cp "$fixture/profiles/fixture.json" "$profiles/fixture.json"

worktree_manifest="$worktree_out/docs/kb/generated/manifest.json"
"$jq_bin" '.generator_version = "shin-generator-fixture-drift"' "$worktree_manifest" \
  >"$work_root/worktree-manifest-drift.json"
mv "$work_root/worktree-manifest-drift.json" "$worktree_manifest"
shin_for fixture "$worktree_root" "$worktree_out" worktree-diff \
  >"$work_root/worktree-generator-drift.json"
assert_json "$work_root/worktree-generator-drift.json" \
  '(.ok | not) and (.current | not) and
   (.comparison.generator_current | not) and .comparison.profile_config_current and
   .summary.metadata_changed == 1 and .summary.total_changes == 1 and
   .details[0].category == "metadata" and .details[0].key == "generator_version"'
shin_for fixture "$worktree_root" "$worktree_out" build \
  >"$work_root/worktree-rebuild.json"

expect_failure "$work_root/worktree-profile-mismatch.json" \
  shin_for empty-extractors "$worktree_root" "$worktree_out" worktree-diff
assert_contains "$work_root/worktree-profile-mismatch.json" \
  '"code": "worktree-diff-profile-mismatch"'

printf '\nWorktree-only architecture evidence.\n' >>"$worktree_root/docs/architecture.md"
printf '\n// Worktree-only source-fact evidence.\n' >>"$worktree_root/src/WidgetCatalog.cs"
cat >"$worktree_root/src/worktree-added.ts" <<'TS'
export function worktreeAdded(value: number): number {
  return value + 1;
}
TS
rm "$worktree_root/src/widget.ts"
shin_for fixture "$worktree_root" "$worktree_out" worktree-diff \
  >"$work_root/worktree-drift.json"
assert_json "$work_root/worktree-drift.json" \
  '.schema_version == "shin.worktree-diff.v1" and (.ok | not) and (.current | not) and
   .summary.metadata_changed == 0 and .summary.documents_changed == 1 and
   .summary.source_facts_added == 1 and .summary.source_facts_changed == 1 and
   .summary.source_facts_removed == 1 and .summary.total_changes == 4 and
   .details_returned == 4 and
   ([.details[] | select(.category == "document" and .change == "changed" and
                         .path == "docs/architecture.md")] | length) == 1 and
   ([.details[] | select(.category == "source-fact" and .change == "changed" and
                         .path == "src/WidgetCatalog.cs")] | length) == 1 and
   ([.details[] | select(.category == "source-fact" and .change == "added" and
                         .path == "src/worktree-added.ts")] | length) == 1 and
   ([.details[] | select(.category == "source-fact" and .change == "removed" and
                         .path == "src/widget.ts")] | length) == 1'
expect_failure "$work_root/worktree-strict.json" \
  shin_for fixture "$worktree_root" "$worktree_out" worktree-diff --strict
assert_json "$work_root/worktree-strict.json" \
  '.schema_version == "shin.worktree-diff.v1" and (.ok | not) and .strict and
   .summary.total_changes == 4'
shin_for fixture "$worktree_root" "$worktree_out" worktree-diff --limit 1 \
  >"$work_root/worktree-limited.json"
assert_json "$work_root/worktree-limited.json" \
  '.detail_limit == 1 and .details_returned == 1 and .details_omitted == 3'
expect_failure "$work_root/worktree-invalid-option.json" \
  shin_for fixture "$worktree_root" "$worktree_out" worktree-diff --unknown
assert_contains "$work_root/worktree-invalid-option.json" '"code": "unsupported-option"'

cat >"$work_root/batch.json" <<'JSON'
{
  "schema_version": "shin.batch-request.v1",
  "requests": [
    {"request_id": "search", "command": "search", "query": "capability", "limit": 4},
    {"request_id": "context", "command": "context", "query": "rollback runbook", "limit": 4, "per_doc_limit": 2},
    {"request_id": "brief", "command": "brief", "query": "result contract", "limit": 6, "per_doc_limit": 2}
  ]
}
JSON

shin batch --input "$work_root/batch.json" --jobs 3 --require-current >"$work_root/batch-result.json"
assert_json "$work_root/batch-result.json" \
  '.schema_version == "shin.batch.v1" and .ok and .execution.snapshot_pinned and .execution.jobs == 3'
assert_json "$work_root/batch-result.json" \
  '[.results[].request_id] == ["search", "context", "brief"]'
batch_snapshot="$($jq_bin -r '.snapshot_id' "$work_root/batch-result.json")"
if ! "$jq_bin" -e --arg snapshot "$batch_snapshot" \
  'all(.results[].result.snapshot_id; . == $snapshot)' "$work_root/batch-result.json" >/dev/null; then
  echo "batch child results do not share the pinned snapshot" >&2
  cat "$work_root/batch-result.json" >&2
  exit 1
fi

shin brief "result contract" --limit 6 --per-doc 2 --require-current >"$work_root/brief.json"
assert_json "$work_root/brief.json" \
  '.ranking_policy == "global-evidence-order-unique-path-v1" and
   .ranked_paths[0].path == "docs/architecture.md" and
   .ranked_paths[1].path == "docs/mixed-authority.md"'
if ! "$jq_bin" -e --slurpfile brief "$work_root/brief.json" \
  '.results[2].result == $brief[0]' "$work_root/batch-result.json" >/dev/null; then
  echo "batch brief result differs from standalone retrieval" >&2
  exit 1
fi

shin batch --input "$work_root/batch.json" --jobs 1 --require-current >"$work_root/batch-serial.json"
if ! "$jq_bin" -e --slurpfile parallel "$work_root/batch-result.json" \
  '.results == $parallel[0].results and .snapshot_id == $parallel[0].snapshot_id' \
  "$work_root/batch-serial.json" >/dev/null; then
  echo "serial and parallel batch results differ" >&2
  exit 1
fi

shin batch --input - --jobs 2 --require-current \
  <"$work_root/batch.json" >"$work_root/batch-stdin.json"
assert_json "$work_root/batch-stdin.json" '.execution.request_count == 3'

cat >"$work_root/batch-invalid.json" <<'JSON'
{"schema_version":"shin.batch-request.v1","requests":[{"request_id":"x","command":"brief","query":"fixture","unknown":true}]}
JSON
missing_out="$work_root/missing-out"
expect_failure "$work_root/batch-invalid-result.json" \
  shin_for fixture "$root" "$missing_out" batch --input "$work_root/batch-invalid.json"
assert_contains "$work_root/batch-invalid-result.json" '"code": "unknown-batch-field"'
if [[ -e "$missing_out/docs" ]]; then
  echo "malformed batch opened or mutated the generated output before validation" >&2
  exit 1
fi

cat >"$work_root/batch-duplicate.json" <<'JSON'
{"schema_version":"shin.batch-request.v1","requests":[
  {"request_id":"same","command":"search","query":"fixture"},
  {"request_id":"same","command":"brief","query":"fixture"}
]}
JSON
expect_failure "$work_root/batch-duplicate-result.json" \
  shin batch --input "$work_root/batch-duplicate.json"
assert_contains "$work_root/batch-duplicate-result.json" '"code": "duplicate-batch-request-id"'

"$jq_bin" -n \
  '{schema_version:"shin.batch-request.v1", requests:[{
    request_id:"bad\u007f", command:"search", query:"fixture"
  }]}' >"$work_root/batch-control-id.json"
expect_failure "$work_root/batch-control-id-result.json" \
  shin batch --input "$work_root/batch-control-id.json"
assert_contains "$work_root/batch-control-id-result.json" '"code": "invalid-batch-request-id"'

"$jq_bin" -n \
  '{schema_version:"shin.batch-request.v1", requests:[range(0;17) |
    {request_id:("r" + tostring), command:"brief", query:"fixture", limit:64, per_doc_limit:8}]}' \
  >"$work_root/batch-budget.json"
expect_failure "$work_root/batch-budget-result.json" \
  shin batch --input "$work_root/batch-budget.json"
assert_contains "$work_root/batch-budget-result.json" '"code": "batch-aggregate-budget-exceeded"'

expect_failure "$work_root/batch-jobs-result.json" \
  shin batch --input "$work_root/batch.json" --jobs 9
assert_contains "$work_root/batch-jobs-result.json" '"code": "invalid-option-value"'

"$jq_bin" -n \
  '{schema_version:"shin.batch-request.v1", requests:[range(0;65) |
    {request_id:("r" + tostring), command:"search", query:"fixture", limit:1}]}' \
  >"$work_root/batch-count.json"
expect_failure "$work_root/batch-count-result.json" \
  shin batch --input "$work_root/batch-count.json"
assert_contains "$work_root/batch-count-result.json" '"code": "batch-too-large"'

dd if=/dev/zero of="$work_root/batch-too-large.json" bs=1048577 count=1 2>/dev/null
expect_failure "$work_root/batch-too-large-result.json" \
  shin batch --input "$work_root/batch-too-large.json"
assert_contains "$work_root/batch-too-large-result.json" '"code": "batch-input-too-large"'

cat >"$work_root/task-request.json" <<'JSON'
{
  "schema_version": "shin.task-request.v1",
  "request_id": "fixture-agent-context",
  "task": {
    "objective": "Load bounded evidence for a fixture implementation task",
    "scope": [
      {"id": "fixture-scope", "query": "capability contract"}
    ],
    "must": [
      {"id": "result-contract", "query": "result contract"},
      {"id": "rollback-runbook", "query": "rollback runbook"}
    ],
    "should": [
      {"id": "widget-source", "query": "triggerWidget arrow-function"}
    ]
  },
  "evidence_types": ["section", "fact", "runbook", "decision", "contract"],
  "budget": {
    "max_evidence_bytes": 24000,
    "max_records": 12,
    "per_query_records": 6,
    "per_document_records": 2
  }
}
JSON

shin pack --input "$work_root/task-request.json" --jobs 4 --require-current --strict \
  >"$work_root/context-pack.json"
assert_json "$work_root/context-pack.json" \
  '.schema_version == "shin.agent-context-pack.v1" and .ok and
   .execution.snapshot_pinned and .execution.jobs == 4 and
   .completion.complete and
   (.completion.required_requirements == .completion.satisfied_required_requirements) and
   ([.requirements[] | select(.required) | .satisfied] | all) and
   (.evidence | length) == .budget.returned_records and
   ([.evidence[].evidence_id] | unique | length) == (.evidence | length) and
   ([.evidence[] | (.record.evidence_kind | type) == "string" and
                    (.record.verification_level | type) == "string"] | all) and
   (.budget | has("pre_dedup_eligible_records") | not) and
   (.budget | has("overlap_deduplicated_records") | not) and
   (.budget.per_document_budget_constrained | type) == "boolean" and
   (.budget.per_document_records as $limit |
      [.evidence[].location.path] | group_by(.) | all(length <= $limit)) and
   ([.evidence[] | select(.location.path == "docs/architecture.md" and
                          .location.start_line == 9 and
                          .evidence_type == "contract" and
                          .record.evidence_kind == "machine-contract")] | length) == 1 and
   ([.evidence[] | .evidence_type as $type |
      ((.matches | length) > 0 and
       (["contract", "decision", "runbook", "fact", "section"] | index($type) != null))] | all) and
   (.evidence | tojson | utf8bytelength) == .budget.evidence_bytes and
   .budget.evidence_bytes <= .budget.max_evidence_bytes and
   (.budget.pressure.level | IN("normal", "warning", "critical")) and
   .budget.pressure.thresholds_basis_points == {warning:8500,critical:9500} and
   .budget.pressure.bytes.used == .budget.evidence_bytes and
   .budget.pressure.bytes.limit == .budget.max_evidence_bytes and
   .budget.pressure.bytes.remaining ==
     (.budget.max_evidence_bytes - .budget.evidence_bytes) and
   .budget.pressure.records.used == .budget.returned_records and
   .budget.pressure.records.limit == .budget.max_records and
   .budget.pressure.records.remaining ==
     (.budget.max_records - .budget.returned_records) and
   (.budget.pressure.top_records | length) > 0 and
   (.budget.pressure.top_documents | length) > 0 and
   ([.budget.pressure.top_records[] | .serialized_bytes > 0 and
      (.requirement_ids | length) > 0] | all) and
   .receipt.snapshot_id == .snapshot_id and
   .receipt.deterministic and (.receipt.cryptographic_authentication | not) and
   (.verification_paths | length) > 0'

cat >"$work_root/task-request-requirement-priority.json" <<'JSON'
{
  "schema_version": "shin.task-request.v1",
  "request_id": "fixture-requirement-priority",
  "task": {
    "objective": "Keep the best evidence for each required query under a shared budget",
    "must": [
      {"id": "alpha-contract", "query": "alpha contract"},
      {"id": "beta-namespace", "query": "beta namespace enrichment"}
    ]
  },
  "evidence_types": ["section"],
  "budget": {
    "max_evidence_bytes": 12000,
    "max_records": 2,
    "per_query_records": 3,
    "per_document_records": 1
  }
}
JSON
shin_for fixture "$requirement_root" "$requirement_out" pack \
  --input "$work_root/task-request-requirement-priority.json" --strict \
  >"$work_root/context-pack-requirement-priority.json"
assert_json "$work_root/context-pack-requirement-priority.json" \
  '.ok and .completion.complete and .budget.returned_records == 2 and
   ([.verification_paths[].path] | index("docs/priority-alpha.md") != null) and
   ([.verification_paths[].path] | index("docs/priority-beta.md") != null) and
   ([.verification_paths[].path] |
      index("docs/priority-z-alpha-secondary.md") == null) and
   ([.requirements[] | select(.id == "alpha-contract") |
      .selected_evidence] == [1]) and
   ([.requirements[] | select(.id == "beta-namespace") |
      .selected_evidence] == [2])'

cat >"$work_root/task-request-document-owner.json" <<'JSON'
{
  "schema_version": "shin.task-request.v1",
  "request_id": "fixture-document-owner-pack",
  "task": {
    "objective": "Resolve one explicitly named document owner",
    "must": [
      {
        "id": "owner",
        "query": "current-handoff exact owner policy integration readiness release evidence"
      }
    ]
  },
  "evidence_types": ["section"],
  "budget": {
    "max_evidence_bytes": 8000,
    "max_records": 1,
    "per_query_records": 4,
    "per_document_records": 1
  }
}
JSON
shin pack --input "$work_root/task-request-document-owner.json" --strict \
  >"$work_root/context-pack-document-owner.json"
assert_json "$work_root/context-pack-document-owner.json" \
  '.ok and .completion.complete and .budget.returned_records == 1 and
   (.verification_paths | length) == 1 and
   .verification_paths[0].path == "docs/current-handoff.md" and
   (.verification_paths[0].ranges | length) == 1 and
   .requirements[0].retrieval_mode == "owner-scoped" and
   .requirements[0].query_diagnostics.matching_policy == "document-owner-scope-v1" and
   ([.verification_paths[].path] | index("docs/handoff-index.md") == null)'

"$jq_bin" '
  .request_id = "fixture-section-only-pack" |
  .task.scope = [] |
  .task.must = [{id:"result-section", query:"result contract"}] |
  .task.should = [] |
  .evidence_types = ["section"] |
  .budget = {max_evidence_bytes:8000, max_records:4,
             per_query_records:4, per_document_records:1}' \
  "$work_root/task-request.json" >"$work_root/task-request-section-only.json"
shin pack --input "$work_root/task-request-section-only.json" --strict \
  >"$work_root/context-pack-section-only.json"
assert_json "$work_root/context-pack-section-only.json" \
  '.ok and .completion.complete and .budget.returned_records > 0 and
   ([.evidence[].evidence_type] | unique) == ["section"] and
   (.budget.per_document_records as $limit |
      [.evidence[].location.path] | group_by(.) | all(length <= $limit))'

"$jq_bin" '.task.scope = [] |
  .task.must = [{id:"partial-coverage", query:"capability result contract missing-evidence"}] |
  .task.should = []' "$work_root/task-request.json" \
  >"$work_root/task-request-partial-coverage.json"
shin pack --input "$work_root/task-request-partial-coverage.json" --strict \
  >"$work_root/context-pack-partial-coverage.json"
assert_json "$work_root/context-pack-partial-coverage.json" \
  '.ok and .completion.complete and (.completion.coverage_complete | not) and
   .completion.required_requirement_ids_with_uncovered_groups == ["partial-coverage"] and
   .requirements[0].evidence_satisfied and .requirements[0].satisfied and
   (.requirements[0].coverage_complete | not) and
   .requirements[0].status == "satisfied-with-uncovered-groups" and
   .requirements[0].coverage_policy.mode == "any-evidence" and
   .requirements[0].selected_query_coverage.covered_groups == 3'

"$jq_bin" '.task.must[0].require_full_coverage = true' \
  "$work_root/task-request-partial-coverage.json" \
  >"$work_root/task-request-full-coverage.json"
expect_failure "$work_root/context-pack-full-coverage.json" \
  shin pack --input "$work_root/task-request-full-coverage.json" --strict
assert_json "$work_root/context-pack-full-coverage.json" \
  '(.ok | not) and (.completion.complete | not) and
   (.completion.coverage_complete | not) and
   .requirements[0].evidence_satisfied and (.requirements[0].satisfied | not) and
   .requirements[0].status == "coverage-incomplete" and
   .requirements[0].coverage_policy.mode == "all-groups"'

"$jq_bin" '.task.must[0].min_covered_groups = 3' \
  "$work_root/task-request-partial-coverage.json" \
  >"$work_root/task-request-minimum-coverage.json"
shin pack --input "$work_root/task-request-minimum-coverage.json" --strict \
  >"$work_root/context-pack-minimum-coverage.json"
assert_json "$work_root/context-pack-minimum-coverage.json" \
  '.ok and .completion.complete and (.completion.coverage_complete | not) and
   .requirements[0].satisfied and
   .requirements[0].coverage_policy.mode == "minimum-groups" and
   .requirements[0].coverage_policy.minimum_covered_groups == 3'

"$jq_bin" '.task.must[0] += {require_full_coverage:true, min_covered_groups:3}' \
  "$work_root/task-request-partial-coverage.json" \
  >"$work_root/task-request-conflicting-coverage.json"
expect_failure "$work_root/context-pack-conflicting-coverage.json" \
  shin pack --input "$work_root/task-request-conflicting-coverage.json"
assert_contains "$work_root/context-pack-conflicting-coverage.json" \
  '"code": "conflicting-task-coverage-policy"'

shin pack --input "$work_root/task-request.json" --jobs 4 --projection compact \
  --telemetry --require-current --strict >"$work_root/context-pack-compact.json"
assert_json "$work_root/context-pack-compact.json" \
  '.schema_version == "shin.agent-context-pack-compact.v1" and .projection == "compact" and
   .ok and .completion.complete and (.gaps | length) == 0 and
   ([.requirements[] |
      ((.query_coverage | has("groups") | not) and
       (.selected_query_coverage | has("schema_version") | not) and
       (.selected_query_coverage | has("covered_group_indices") | not) and
       (.selected_query_coverage | has("uncovered_group_indices")) and
       (has("evidence_satisfied") | not) and
       (has("coverage_complete") | not) and
       (has("query") | not))] | all) and
   .telemetry.schema_version == "shin.pack-telemetry.v1" and
   (.telemetry.phases_ms.session_open | type) == "number" and
   (.telemetry.phases_ms.retrieval | type) == "number" and
   (.telemetry.phases_ms.selection | type) == "number" and
   (.telemetry.phases_ms.projection | type) == "number" and
   (.telemetry.phases_ms.serialization | type) == "number" and
   .telemetry.process_peak_rss_bytes > 0 and
   .telemetry.projected_payload_bytes > .budget.evidence_bytes'
if ! "$jq_bin" -e --slurpfile full "$work_root/context-pack.json" \
  '.evidence == $full[0].evidence and
   .verification_paths == $full[0].verification_paths and
   .budget == $full[0].budget and
   .receipt.receipt_id == $full[0].receipt.receipt_id and
   .receipt.evidence_set_fingerprint == $full[0].receipt.evidence_set_fingerprint' \
  "$work_root/context-pack-compact.json" >/dev/null; then
  echo "compact and full projections do not share one selected evidence set" >&2
  exit 1
fi
full_pack_bytes="$(wc -c <"$work_root/context-pack.json" | tr -d ' ')"
compact_payload_bytes="$($jq_bin -r '.telemetry.projected_payload_bytes' \
  "$work_root/context-pack-compact.json")"
if (( compact_payload_bytes >= full_pack_bytes )); then
  echo "compact projection did not reduce the fixture pack payload" >&2
  exit 1
fi

shin pack-bench --input "$work_root/task-request.json" --jobs 4 --projection compact \
  --iterations 3 --warmup 0 --strict >"$work_root/context-pack-bench.json"
assert_json "$work_root/context-pack-bench.json" \
  '.schema_version == "shin.pack-bench.v1" and .ok and
   .receipt_stable and .complete and .completion_stable and
   .iterations == 3 and .jobs == 4 and
   .phase_latency_ms.total.p50 > 0 and .phase_latency_ms.session_open.p50 >= 0 and
   .output_bytes.min > 0 and .process_peak_rss_bytes > 0'

cat >"$work_root/task-request-incomplete.json" <<'JSON'
{
  "schema_version": "shin.task-request.v1",
  "request_id": "fixture-incomplete-pack",
  "task": {
    "objective": "Exercise stable incomplete Pack benchmark semantics",
    "must": [
      {"id": "missing-evidence", "query": "definitelyabsentpackterm"}
    ]
  }
}
JSON

shin pack-bench --input "$work_root/task-request-incomplete.json" --iterations 3 \
  --warmup 0 >"$work_root/context-pack-bench-incomplete.json"
assert_json "$work_root/context-pack-bench-incomplete.json" \
  '.schema_version == "shin.pack-bench.v1" and .ok and
   .receipt_stable and (.complete | not) and .completion_stable'
expect_failure "$work_root/context-pack-bench-incomplete-strict.json" \
  shin pack-bench --input "$work_root/task-request-incomplete.json" \
    --iterations 3 --warmup 0 --strict
assert_json "$work_root/context-pack-bench-incomplete-strict.json" \
  '.schema_version == "shin.pack-bench.v1" and (.ok | not) and
   .receipt_stable and (.complete | not) and .completion_stable'

shin pack-eval --strict >"$work_root/context-pack-eval.json"
assert_json "$work_root/context-pack-eval.json" \
  '.schema_version == "shin.pack-eval.v1" and .ok and
   .summary == {"failed":0,"passed":3,"total":3} and
   (.cases | length) == 3 and
   all(.cases[];
     .passed and .serial_parallel.identical and
     .limits.compact_bytes <= .limits.max_compact_bytes)'

shin validate >"$work_root/validate.json"
assert_json "$work_root/validate.json" \
  '.schema_version == "shin.validation.v1" and .ok and
   .execution.policy == "independent-complete-diagnostics-v1" and
   (.execution.fail_fast | not) and (.execution.index_mutation | not) and
   .summary.total == 7 and .summary.attempted == 6 and .summary.passed == 6 and
   .summary.failed == 0 and .summary.skipped == 1 and
   .summary.failed_by_domain == {} and
   .summary.failure_classes == {safety_findings:0,configuration_failures:0,
     index_integrity_failures:0,retrieval_quality_failures:0,
     adoption_failures:0,runtime_failures:0} and
   ([.gates[] | select(.attempted) | .ok] | all) and
   (.gates[] | select(.id == "adoption-check") | .status) == "skipped"'

shin validate --summary >"$work_root/validate-summary.json"
assert_json "$work_root/validate-summary.json" \
  '.schema_version == "shin.validation-summary.v2" and .projection == "summary" and
   .ok and .summary.total == 7 and .summary.passed == 6 and .summary.skipped == 1 and
   ([.gates[] | has("result") or has("error")] | any | not) and
   (.gates[] | select(.id == "adoption-check") | .status) == "skipped"'
for result in validate.json validate-summary.json; do
  assert_json "$work_root/$result" \
    'all(.gates[]; if .attempted then ((.elapsed_ms | type) == "number" and .elapsed_ms >= 0) else .elapsed_ms == null end)'
done
full_validate_bytes="$(wc -c <"$work_root/validate.json" | tr -d ' ')"
summary_validate_bytes="$(wc -c <"$work_root/validate-summary.json" | tr -d ' ')"
if (( summary_validate_bytes >= full_validate_bytes )); then
  echo "validate --summary did not reduce the successful validation envelope" >&2
  exit 1
fi

expect_failure "$work_root/validate-strict.json" shin validate --strict
assert_json "$work_root/validate-strict.json" \
  '(.ok | not) and .summary.attempted == 6 and .summary.passed == 5 and
   .summary.failed == 1 and .summary.failed_by_domain == {configuration:1} and
   .summary.failure_classes.configuration_failures == 1 and
   .summary.failure_classes.safety_findings == 0 and
   .summary.failure_classes.retrieval_quality_failures == 0 and
   ([.gates[] | select(.attempted)] | length) == 6 and
   (.gates[] | select(.id == "profile-check") | .status) == "failed" and
   ([.gates[] | select(.domain == "retrieval-quality") | .ok] | all)'

expect_failure "$work_root/validate-strict-summary.json" shin validate --strict --summary
assert_json "$work_root/validate-strict-summary.json" \
  '.schema_version == "shin.validation-summary.v2" and .projection == "summary" and
   (.ok | not) and .summary.failed == 1 and
   ([.gates[] | select(.status == "passed") | has("result") or has("error")] | any | not) and
   (.gates[] | select(.id == "profile-check") |
      .status == "failed" and (.result | type) == "object" and .error == null)'

assert_json "$work_root/validate-strict-summary.json" \
  'all(.gates[] | select(.attempted); (.elapsed_ms | type) == "number" and .elapsed_ms >= 0)'

expect_failure "$work_root/validate-unknown-option.json" shin validate --telemetry
assert_contains "$work_root/validate-unknown-option.json" '"code": "unsupported-option"'
expect_failure "$work_root/validate-duplicate-option.json" shin validate --strict --strict
assert_contains "$work_root/validate-duplicate-option.json" '"code": "duplicate-option"'
expect_failure "$work_root/validate-duplicate-summary.json" shin validate --summary --summary
assert_contains "$work_root/validate-duplicate-summary.json" '"code": "duplicate-option"'

"$jq_bin" '
  .id = "failing" |
  .sourceLabel = "Bounded failure projection fixture" |
  .evalCases = [{
    id: "missing-owner",
    query: "current-handoff exact owner policy integration readiness release evidence",
    mustPaths: ["docs/operations.md"],
    mustTopPaths: ["docs/operations.md"],
    mustTopN: 1,
    limit: 4,
    perDocLimit: 1
  }] |
  .packEvalCases = [{
    id: "missing-pack-owner",
    request: {
      schema_version: "shin.task-request.v1",
      request_id: "missing-pack-owner",
      task: {objective: "Exercise bounded Pack failures", must: [{id:"owner", query:"current-handoff exact owner policy integration readiness release evidence"}]},
      budget: {max_evidence_bytes:8000, max_records:2, per_query_records:4, per_document_records:1}
    },
    mustPaths: ["docs/operations.md"],
    expectedComplete: true,
    maxEvidenceBytes: 8000,
    maxRecords: 2,
    maxCompactBytes: 12000,
    requireSerialParallelIdentity: true
  }]' "$fixture/profiles/fixture.json" >"$profiles/failing.json"
failing_out="$work_root/failing-out"
shin_for failing "$root" "$failing_out" build >"$work_root/failing-build.json"
expect_failure "$work_root/eval-failures.json" \
  shin_for failing "$root" "$failing_out" eval --failed-only --require-current
assert_json "$work_root/eval-failures.json" \
  '.schema_version == "shin.eval-failures.v1" and .projection == "failed-only" and
   (.ok | not) and .failure_budget.eligible_records == 1 and
   .failure_budget.omitted_records == 0 and
   .failure_budget.detail_records_omitted == 0 and
   (.failures | length) == 1 and .failures[0].id == "missing-owner" and
   .failures[0].detail_level == "full" and
   .failures[0].missing_paths == ["docs/operations.md"] and
   (.failures[0].diagnose[0].argv | index("diagnose")) != null and
   (.failures[0] |
      if has("identifier_scope_used") then
        (.matching_policy | type) == "string" and
        .identifier_scope_stage == "selection-discovery" and
        (.identifier_scope_used | type) == "boolean" and
        (.identifier_scope_key | type) == "string" and
        (.identifier_probe_rows | type) == "number" and
        (if .identifier_scope_used then
           .retrieval_mode == "identifier-scoped" and
           .matching_policy == "confirmed-identifier-scope-v1"
         else
           .retrieval_mode != "identifier-scoped" and
           .matching_policy != "confirmed-identifier-scope-v1"
         end)
      else true end)'
expect_failure "$work_root/pack-eval-failures.json" \
  shin_for failing "$root" "$failing_out" pack-eval --failed-only --require-current
assert_json "$work_root/pack-eval-failures.json" \
  '.schema_version == "shin.pack-eval-failures.v1" and
   .projection == "failed-only" and (.ok | not) and
   .summary.failed == 1 and (.cases | length) == 1 and
   .cases[0].id == "missing-pack-owner" and
   .cases[0].detail_level == "full" and
   .cases[0].missing_paths == ["docs/operations.md"] and
   .cases[0].requirement_queries[0].id == "owner" and
   (.cases[0].remediation.diagnose_candidates[0].argv | index("diagnose")) != null'
expect_failure "$work_root/validate-failures-summary.json" \
  shin_for failing "$root" "$failing_out" validate --summary --require-current
assert_json "$work_root/validate-failures-summary.json" \
  '.schema_version == "shin.validation-summary.v2" and (.ok | not) and
   .summary.failure_classes.retrieval_quality_failures == 2 and
   ([.gates[] | select(.domain == "retrieval-quality" and .status == "failed")] | length) == 2 and
   (.gates[] | select(.id == "eval") | .result.schema_version) == "shin.eval-failures.v1" and
   (.gates[] | select(.id == "pack-eval") | .result.schema_version) == "shin.pack-eval-failures.v1" and
   (.gates[] | select(.id == "eval") | .result.detail_command.argv | index("eval")) != null and
   (.gates[] | select(.id == "eval") | .result.detail_command.argv | index("--profile")) != null and
   ([.gates[] | select(.status == "failed") | .result |
      (tojson | utf8bytelength) <= .diagnostic_budget.max_bytes] | all)'
if (( $(wc -c <"$work_root/validate-failures-summary.json") > 65536 )); then
  echo "validate --summary failure envelope exceeded 64 KiB" >&2
  exit 1
fi
expect_failure "$work_root/ready-validation-failures.json" \
  shin_for failing "$root" "$failing_out" ready
assert_json "$work_root/ready-validation-failures.json" \
  '(.ok | not) and .next_action == "inspect-validation-failures" and
   (.next_command.argv | index("validate")) != null and
   (.next_command.argv | index("--summary")) == null and
   (.steps[] | select(.id == "validate") |
      (.result.failed_gates | length) == 2 and
      ([.result.failed_gates[].diagnostic.schema_version] |
       index("shin.eval-failures.v1") != null))'
if (( $(wc -c <"$work_root/ready-validation-failures.json") > 65536 )); then
  echo "ready failure envelope exceeded 64 KiB" >&2
  exit 1
fi

"$jq_bin" '
  .id = "oversized-failure" |
  ("o" * 14000) as $long_query |
  .evalCases[0].query = $long_query |
  .evalCases[0].minCoveredGroups = 1 |
  .packEvalCases[0].request.task.must[0].query = $long_query
' "$profiles/failing.json" >"$profiles/oversized-failure.json"
oversized_out="$work_root/oversized-failure-out"
shin_for oversized-failure "$root" "$oversized_out" build \
  >"$work_root/oversized-failure-build.json"
expect_failure "$work_root/eval-failure-header.json" \
  shin_for oversized-failure "$root" "$oversized_out" eval --failed-only --require-current
assert_json "$work_root/eval-failure-header.json" \
  '.schema_version == "shin.eval-failures.v1" and (.ok | not) and
   .failure_budget.eligible_records == 1 and .failure_budget.returned_records == 1 and
   .failure_budget.omitted_records == 0 and .failure_budget.detail_records_omitted == 1 and
   .failure_budget.truncated and .failure_budget.used_bytes <= .failure_budget.max_bytes and
   .failures[0].id == "missing-owner" and .failures[0].detail_level == "header" and
   .failures[0].details_omitted and
   .failures[0].ceiling_violation_details ==
     [{metric:"covered_groups", actual:0, allowed:1, comparator:">=", stage:"coverage"}]'
expect_failure "$work_root/pack-eval-failure-header.json" \
  shin_for oversized-failure "$root" "$oversized_out" pack-eval --failed-only --require-current
assert_json "$work_root/pack-eval-failure-header.json" \
  '.schema_version == "shin.pack-eval-failures.v1" and (.ok | not) and
   .failure_budget.eligible_records == 1 and .failure_budget.returned_records == 1 and
   .failure_budget.omitted_records == 0 and .failure_budget.detail_records_omitted == 1 and
   .failure_budget.truncated and .failure_budget.used_bytes <= .failure_budget.max_bytes and
   .cases[0].id == "missing-pack-owner" and .cases[0].detail_level == "header" and
   .cases[0].details_omitted'

stale_root="$work_root/stale-root"
stale_out="$work_root/stale-out"
cp -R "$root" "$stale_root"
shin_for failing "$stale_root" "$stale_out" build >"$work_root/stale-build.json"
printf '\nObserved after the published snapshot.\n' >>"$stale_root/docs/current-handoff.md"
shin_for failing "$stale_root" "$stale_out" eval --failed-only \
  >"$work_root/non-strict-stale-eval.json"
assert_json "$work_root/non-strict-stale-eval.json" \
  '(.ok | not) and (.suite_failure_reasons | index("query-failures")) != null and
   (.suite_failure_reasons | index("index-not-current")) == null and
   (.suite_failure_reasons | index("index-not-fully-verified")) == null'
shin_for failing "$stale_root" "$stale_out" pack-eval --failed-only \
  >"$work_root/non-strict-stale-pack-eval.json"
assert_json "$work_root/non-strict-stale-pack-eval.json" \
  '(.ok | not) and (.suite_failure_reasons | index("case-failures")) != null and
   (.suite_failure_reasons | index("index-not-current")) == null and
   (.suite_failure_reasons | index("index-not-fully-verified")) == null'

cat >"$work_root/task-request-reordered.json" <<'JSON'
{
  "budget": {
    "per_document_records": 2,
    "per_query_records": 6,
    "max_records": 12,
    "max_evidence_bytes": 24000
  },
  "evidence_types": ["fact", "contract", "section", "decision", "runbook"],
  "task": {
    "should": [
      {"query": "triggerWidget arrow-function", "id": "widget-source"}
    ],
    "must": [
      {"query": "result contract", "id": "result-contract"},
      {"query": "rollback runbook", "id": "rollback-runbook"}
    ],
    "scope": [
      {"query": "capability contract", "id": "fixture-scope"}
    ],
    "objective": "Load bounded evidence for a fixture implementation task"
  },
  "request_id": "fixture-agent-context",
  "schema_version": "shin.task-request.v1"
}
JSON

shin pack --input "$work_root/task-request-reordered.json" --jobs 1 --require-current --strict \
  >"$work_root/context-pack-reordered.json"
if ! "$jq_bin" -e --slurpfile original "$work_root/context-pack.json" \
  '.receipt == $original[0].receipt and
   .evidence == $original[0].evidence and
   .verification_paths == $original[0].verification_paths' \
  "$work_root/context-pack-reordered.json" >/dev/null; then
  echo "canonical task requests produced different context pack evidence" >&2
  exit 1
fi

"$jq_bin" '.budget.max_evidence_bytes = 2' "$work_root/task-request.json" \
  >"$work_root/task-request-tiny.json"
expect_failure "$work_root/context-pack-tiny-strict.json" \
  shin pack --input "$work_root/task-request-tiny.json" --strict
assert_json "$work_root/context-pack-tiny-strict.json" \
  '(.ok | not) and (.completion.complete | not) and
   .budget.evidence_bytes == 2 and .budget.byte_budget_constrained and
   .budget.pressure.level == "critical" and .budget.pressure.warning and
   .budget.pressure.warning_codes == ["pack-budget-constrained"] and
   .budget.pressure.bytes == {used:2,limit:2,remaining:0,
     utilization_basis_points:10000} and
   (.budget.pressure.top_records | length) == 0 and
   (.budget.pressure.top_documents | length) == 0 and
   (.evidence | length) == 0 and
   (.completion.unsatisfied_required_requirement_ids | length) == 3'
expect_failure "$work_root/context-pack-tiny-compact.json" \
  shin pack --input "$work_root/task-request-tiny.json" --projection compact --strict
assert_json "$work_root/context-pack-tiny-compact.json" \
  '(.ok | not) and (.completion.complete | not) and
   (.requirements | length) == 4 and (.gaps | length) == 4 and
   ([.gaps[] | select(.required)] | length) == 3 and
   ([.gaps[] | (.query_coverage.groups | length) > 0] | all)'
shin pack --input "$work_root/task-request-tiny.json" \
  >"$work_root/context-pack-tiny-nonstrict.json"
assert_json "$work_root/context-pack-tiny-nonstrict.json" \
  '.ok and (.completion.complete | not) and (.evidence | length) == 0'

"$jq_bin" '.budget.max_records = 1' "$work_root/task-request.json" \
  >"$work_root/task-request-one-record.json"
expect_failure "$work_root/context-pack-one-record.json" \
  shin pack --input "$work_root/task-request-one-record.json" --strict
assert_json "$work_root/context-pack-one-record.json" \
  '(.ok | not) and (.completion.complete | not) and
   .budget.returned_records == 1 and .budget.record_budget_constrained and
   .budget.pressure.level == "critical" and
   .budget.pressure.dominant_dimension == "records" and
   .budget.pressure.records.utilization_basis_points == 10000 and
   (.completion.unsatisfied_required_requirement_ids | length) > 0'

"$jq_bin" '.budget.max_evidence_bytes = 4194304 | .budget.max_records = 128 |
  .budget.per_document_records = 8' \
  "$work_root/task-request.json" >"$work_root/task-request-headroom.json"
shin pack --input "$work_root/task-request-headroom.json" --strict \
  >"$work_root/context-pack-headroom.json"
assert_json "$work_root/context-pack-headroom.json" \
  '.ok and .completion.complete and .budget.pressure.level == "normal" and
   (.budget.pressure.warning | not) and
   .budget.pressure.warning_codes == [] and
   .budget.pressure.bytes.utilization_basis_points < 8500 and
   .budget.pressure.records.utilization_basis_points < 8500'

"$jq_bin" '.budget.max_evidence_bytes = 4194304 | .budget.max_records = 128 |
  .budget.per_document_records = 1' "$work_root/task-request.json" \
  >"$work_root/task-request-document-pressure.json"
shin pack --input "$work_root/task-request-document-pressure.json" --strict \
  >"$work_root/context-pack-document-pressure.json"
assert_json "$work_root/context-pack-document-pressure.json" \
  '.ok and .completion.complete and .budget.per_document_budget_constrained and
   .budget.pressure.level == "critical" and .budget.pressure.warning and
   .budget.pressure.warning_codes == ["pack-budget-constrained"] and
   .budget.pressure.bytes.utilization_basis_points < 8500 and
   .budget.pressure.records.utilization_basis_points < 8500'

"$jq_bin" '.task.scope = [] | .task.must = [{id:"section-only", query:"result contract"}] |
  .task.should = [] | .evidence_types = ["section"]' "$work_root/task-request.json" \
  >"$work_root/task-request-sections.json"
shin pack --input "$work_root/task-request-sections.json" --strict \
  >"$work_root/context-pack-sections.json"
assert_json "$work_root/context-pack-sections.json" \
  '.completion.complete and (.evidence | length) > 0 and
   (.evidence | all(.evidence_type == "section"))'

cat >"$work_root/task-request-invalid.json" <<'JSON'
{"schema_version":"shin.task-request.v1","request_id":"invalid","task":{"objective":"fixture","must":[{"id":"x","query":"fixture","unknown":true}]}}
JSON
missing_pack_out="$work_root/missing-pack-out"
expect_failure "$work_root/context-pack-invalid-result.json" \
  shin_for fixture "$root" "$missing_pack_out" pack --input "$work_root/task-request-invalid.json"
assert_contains "$work_root/context-pack-invalid-result.json" '"code": "unknown-task-field"'
if [[ -e "$missing_pack_out/docs" ]]; then
  echo "malformed task opened or mutated the generated output before validation" >&2
  exit 1
fi

"$jq_bin" '.task.must += [{id:"result-contract", query:"fixture"}]' \
  "$work_root/task-request.json" >"$work_root/task-request-duplicate.json"
expect_failure "$work_root/context-pack-duplicate-result.json" \
  shin pack --input "$work_root/task-request-duplicate.json"
assert_contains "$work_root/context-pack-duplicate-result.json" \
  '"code": "duplicate-task-requirement-id"'

"$jq_bin" '.task.must = []' "$work_root/task-request.json" \
  >"$work_root/task-request-no-must.json"
expect_failure "$work_root/context-pack-no-must-result.json" \
  shin pack --input "$work_root/task-request-no-must.json"
assert_contains "$work_root/context-pack-no-must-result.json" '"code": "task-must-required"'

"$jq_bin" '.evidence_types = ["section", "unknown"]' "$work_root/task-request.json" \
  >"$work_root/task-request-invalid-type.json"
expect_failure "$work_root/context-pack-invalid-type-result.json" \
  shin pack --input "$work_root/task-request-invalid-type.json"
assert_contains "$work_root/context-pack-invalid-type-result.json" \
  '"code": "invalid-task-evidence-type"'

expect_failure "$work_root/context-pack-jobs-result.json" \
  shin pack --input "$work_root/task-request.json" --jobs 9
assert_contains "$work_root/context-pack-jobs-result.json" '"code": "invalid-option-value"'

expect_failure "$work_root/context-pack-projection-result.json" \
  shin pack --input "$work_root/task-request.json" --projection tiny
assert_contains "$work_root/context-pack-projection-result.json" '"code": "invalid-option-value"'

expect_failure "$work_root/context-pack-bench-input-result.json" shin pack-bench
assert_contains "$work_root/context-pack-bench-input-result.json" '"code": "missing-option"'

dd if=/dev/zero of="$work_root/task-too-large.json" bs=1048577 count=1 2>/dev/null
expect_failure "$work_root/context-pack-too-large-result.json" \
  shin pack --input "$work_root/task-too-large.json"
assert_contains "$work_root/context-pack-too-large-result.json" '"code": "task-input-too-large"'

shin diagnose "result contract" --expected-path docs/architecture.md \
  --limit 6 --per-doc 2 --require-current --strict >"$work_root/diagnose-returned.json"
assert_json "$work_root/diagnose-returned.json" \
  '.ok and .expectation.met and .returned_path_rank == 1 and
   (.budget_probe.executed | not) and .budget_probe.returned_path_rank == null and
   .indexed_rows.probe_complete and .source.published_document and
   .matching.best_evidence_kind == "machine-contract" and
   .matching.best_verification_level == "declared" and
   .ranking_policy == "global-evidence-order-unique-path-v1"'
if ! "$jq_bin" -e --slurpfile brief "$work_root/brief.json" \
  '.retrieval.ranked_paths == $brief[0].ranked_paths' \
  "$work_root/diagnose-returned.json" >/dev/null; then
  echo "diagnose and brief rank tables differ" >&2
  exit 1
fi

shin diagnose "result contract" --expected-path docs/mixed-authority.md \
  --limit 1 --per-doc 1 >"$work_root/diagnose-ranked-out.json"
assert_json "$work_root/diagnose-ranked-out.json" \
  '(.ok | not) and (.expectation.reason_codes | index("returned-with-expanded-budget") != null) and
   .budget_probe.executed and (.budget_probe.route_changed | not) and
   .budget_probe.returned_path_rank == 2'

shin diagnose "result contract" --expected-path docs/current-context.md \
  >"$work_root/diagnose-unmatched.json"
assert_json "$work_root/diagnose-unmatched.json" \
  '(.ok | not) and .source.published_document and .indexed_rows.total > 0 and
   (.expectation.reason_codes | index("no-query-groups-matched") != null)'

shin diagnose "triggerWidget arrow-function" --expected-path src/widget.ts \
  --strict >"$work_root/diagnose-source-fact.json"
assert_json "$work_root/diagnose-source-fact.json" \
  '.ok and .source.source_fact_configured and .source.published_source_fact and
   (.indexed_rows.facts > 0) and
   .matching.best_evidence_kind == "implementation-source" and
   .matching.best_verification_level == "source-observed"'

shin diagnose discarded --expected-path docs/excluded/ignored.jsonl \
  >"$work_root/diagnose-excluded.json"
assert_json "$work_root/diagnose-excluded.json" \
  '(.ok | not) and .source.exists and (.source.document_policy_allowed | not) and
   (.expectation.reason_codes | index("excluded-by-profile") != null)'

shin diagnose fixture --expected-path docs/does-not-exist.md \
  >"$work_root/diagnose-missing.json"
assert_json "$work_root/diagnose-missing.json" \
  '(.ok | not) and (.expectation.reason_codes | index("source-missing") != null) and
   (.expectation.reason_codes | index("not-in-published-snapshot") != null)'
expect_failure "$work_root/diagnose-missing-strict.json" \
  shin diagnose fixture --expected-path docs/does-not-exist.md --strict
assert_json "$work_root/diagnose-missing-strict.json" '.strict and (.ok | not)'

expect_failure "$work_root/diagnose-traversal.json" \
  shin diagnose fixture --expected-path ../outside.md
assert_contains "$work_root/diagnose-traversal.json" '"code": "invalid-expected-path"'
expect_failure "$work_root/diagnose-noncanonical.json" \
  shin diagnose fixture --expected-path docs/../README.md
assert_contains "$work_root/diagnose-noncanonical.json" '"code": "invalid-expected-path"'
expect_failure "$work_root/diagnose-retired-alias.json" \
  shin diagnose fixture --expect-path README.md
assert_contains "$work_root/diagnose-retired-alias.json" '"code": "missing-option"'

retrieval_symlink_out="$work_root/out-retrieval-symlink"
cp -R "$out" "$retrieval_symlink_out"
mv "$retrieval_symlink_out/docs/kb/generated/manifest.json" \
  "$retrieval_symlink_out/docs/kb/generated/manifest-real.json"
ln -s manifest-real.json "$retrieval_symlink_out/docs/kb/generated/manifest.json"
expect_failure "$work_root/retrieval-symlink.json" \
  shin_for fixture "$root" "$retrieval_symlink_out" batch --input "$work_root/batch.json"
assert_contains "$work_root/retrieval-symlink.json" '"code": "snapshot-file-invalid"'
expect_failure "$work_root/context-pack-retrieval-symlink.json" \
  shin_for fixture "$root" "$retrieval_symlink_out" pack \
    --input "$work_root/task-request.json"
assert_contains "$work_root/context-pack-retrieval-symlink.json" \
  '"code": "snapshot-file-invalid"'

shin eval --strict >"$work_root/eval.json"
assert_json "$work_root/eval.json" \
  '.rank_summary.policy == "global-evidence-order-unique-path-v1" and
   (.rank_summary.mean_reciprocal_rank as $actual |
    ([.cases[] | ., .query_variants[]?] | map(.rank_metrics.reciprocal_rank) | add / length) as $expected |
    (($actual - $expected | fabs) < 0.000001)) and
   ([.cases[] | select(.id == "fixture-authority-rank")][0].rank_assertions[0].passed) and
   ([.cases[] | . as $case | $case, $case.query_variants[]] | all(.[];
      . as $case | .query_diagnostics |
      if has("identifier_scope_used") then
        .identifier_scope_stage == "selection-discovery" and
        (.identifier_scope_used | type) == "boolean" and
        (.identifier_scope_key | type) == "string" and
        (.identifier_probe_rows | type) == "number" and
        (if .identifier_scope_used then
           $case.retrieval_mode == "identifier-scoped" and
           .matching_policy == "confirmed-identifier-scope-v1"
         else
           $case.retrieval_mode != "identifier-scoped" and
           .matching_policy != "confirmed-identifier-scope-v1"
         end)
      else true end))'
if ! "$jq_bin" -e --slurpfile brief "$work_root/brief.json" \
  '([.cases[] | select(.id == "fixture-authority-rank")][0].ranked_paths) ==
   $brief[0].ranked_paths' "$work_root/eval.json" >/dev/null; then
  echo "eval and brief rank tables differ" >&2
  exit 1
fi

"$jq_bin" '{
  matched_sections: ([.cases[] | select(.id == "fixture-capability-contract")][0].matched_sections),
  candidate_rows: ([.cases[] | select(.id == "fixture-capability-contract")][0].query_diagnostics.candidate_rows),
  covered_groups: ([.cases[] | select(.id == "fixture-capability-contract")][0].query_coverage.covered_groups),
  matched_facts: ([.cases[] | select(.id == "fixture-widget-source-fact")][0].matched_facts)
}' "$work_root/eval.json" >"$work_root/ceiling-metrics.json"
assert_json "$work_root/ceiling-metrics.json" \
  '.matched_sections > 0 and .candidate_rows > 0 and .covered_groups > 0 and .matched_facts > 0'

"$jq_bin" --slurpfile metrics "$work_root/ceiling-metrics.json" '
  $metrics[0] as $metrics |
  .id = "ceiling-boundary" |
  .evalCases = [
    (.evalCases[] | select(.id == "fixture-capability-contract") |
      .id = "ceiling-matches-candidates-coverage-boundary" |
      .queryVariants = [] |
      .maxMatchedSections = $metrics.matched_sections |
      .maxCandidateRows = $metrics.candidate_rows |
      .minCoveredGroups = $metrics.covered_groups),
    (.evalCases[] | select(.id == "fixture-widget-source-fact") |
      .id = "ceiling-facts-boundary" |
      .maxMatchedFacts = $metrics.matched_facts)
  ] |
  .packEvalCases = []
' "$fixture/profiles/fixture.json" >"$profiles/ceiling-boundary.json"
ceiling_boundary_out="$work_root/ceiling-boundary-out"
shin_for ceiling-boundary "$root" "$ceiling_boundary_out" build \
  >"$work_root/ceiling-boundary-build.json"
shin_for ceiling-boundary "$root" "$ceiling_boundary_out" eval --strict \
  >"$work_root/ceiling-boundary-eval.json"
assert_json "$work_root/ceiling-boundary-eval.json" \
  '.ok and
   ([.cases[] | select(.id == "ceiling-matches-candidates-coverage-boundary")][0] |
      .matched_sections == .max_matched_sections and
      .query_diagnostics.candidate_rows == .max_candidate_rows and
      .query_coverage.covered_groups == .min_covered_groups and
      .ceiling_violations == [] and .ceiling_violation_details == []) and
   ([.cases[] | select(.id == "ceiling-facts-boundary")][0] |
      .matched_facts == .max_matched_facts and
      .ceiling_violations == [] and .ceiling_violation_details == [])'

"$jq_bin" --slurpfile metrics "$work_root/ceiling-metrics.json" '
  $metrics[0] as $metrics |
  .id = "ceiling-failures" |
  .evalCases = [
    (.evalCases[] | select(.id == "fixture-capability-contract") |
      .id = "ceiling-matches-candidates-coverage-failure" |
      .queryVariants = [] |
      .maxMatchedSections = ($metrics.matched_sections - 1) |
      .maxCandidateRows = ($metrics.candidate_rows - 1) |
      .minCoveredGroups = ($metrics.covered_groups + 1)),
    (.evalCases[] | select(.id == "fixture-widget-source-fact") |
      .id = "ceiling-facts-failure" |
      .maxMatchedFacts = ($metrics.matched_facts - 1))
  ] |
  .packEvalCases = []
' "$fixture/profiles/fixture.json" >"$profiles/ceiling-failures.json"
"$jq_bin" --slurpfile metrics "$work_root/ceiling-metrics.json" -n '
  $metrics[0] as $metrics |
  [
    [
      {metric:"matched_sections", actual:$metrics.matched_sections,
       allowed:($metrics.matched_sections - 1), comparator:"<=", stage:"scored-matches"},
      {metric:"candidate_rows", actual:$metrics.candidate_rows,
       allowed:($metrics.candidate_rows - 1), comparator:"<=", stage:"candidate-discovery"},
      {metric:"covered_groups", actual:$metrics.covered_groups,
       allowed:($metrics.covered_groups + 1), comparator:">=", stage:"coverage"}
    ],
    [
      {metric:"matched_facts", actual:$metrics.matched_facts,
       allowed:($metrics.matched_facts - 1), comparator:"<=", stage:"scored-matches"}
    ]
  ]
' >"$work_root/ceiling-violation-details.json"
ceiling_failures_out="$work_root/ceiling-failures-out"
shin_for ceiling-failures "$root" "$ceiling_failures_out" build \
  >"$work_root/ceiling-failures-build.json"
expect_failure "$work_root/ceiling-failures-eval.json" \
  shin_for ceiling-failures "$root" "$ceiling_failures_out" eval --strict
if ! "$jq_bin" -e --slurpfile expected "$work_root/ceiling-violation-details.json" \
  '(.ok | not) and
   [.cases[] | .ceiling_violation_details] == $expected[0] and
   [.cases[] | .ceiling_violations] ==
     [["matched_sections", "candidate_rows", "covered_groups"], ["matched_facts"]]' \
  "$work_root/ceiling-failures-eval.json" >/dev/null; then
  echo "eval ceiling diagnostics did not retain exact violation details" >&2
  cat "$work_root/ceiling-failures-eval.json" >&2
  exit 1
fi
expect_failure "$work_root/ceiling-failures-projection.json" \
  shin_for ceiling-failures "$root" "$ceiling_failures_out" eval --failed-only --require-current
if ! "$jq_bin" -e --slurpfile expected "$work_root/ceiling-violation-details.json" \
  '(.ok | not) and .failure_budget.eligible_records == 2 and
   [.failures[] | .ceiling_violation_details] == $expected[0] and
   [.failures[] | .ceiling_violations] ==
     [["matched_sections", "candidate_rows", "covered_groups"], ["matched_facts"]]' \
  "$work_root/ceiling-failures-projection.json" >/dev/null; then
  echo "failed-only ceiling diagnostics did not retain exact violation details" >&2
  cat "$work_root/ceiling-failures-projection.json" >&2
  exit 1
fi

"$jq_bin" '.id = "rank-reversed" | .evalCases = [{
  id:"reversed", query:"result contract", mustTopN:3, limit:6, perDocLimit:2,
  mustRankBefore:[{higher:"docs/mixed-authority.md", lower:"docs/architecture.md"}]
}]' "$fixture/profiles/fixture.json" >"$profiles/rank-reversed.json"
rank_reversed_out="$work_root/rank-reversed-out"
shin_for rank-reversed "$root" "$rank_reversed_out" build >"$work_root/rank-reversed-build.json"
expect_failure "$work_root/rank-reversed-eval.json" \
  shin_for rank-reversed "$root" "$rank_reversed_out" eval --strict
assert_json "$work_root/rank-reversed-eval.json" \
  '(.ok | not) and .cases[0].rank_violations[0].reason == "order-violated"'

"$jq_bin" '.id = "rank-lower-absent" | .evalCases = [{
  id:"lower-absent", query:"result contract", mustTopN:3, limit:6, perDocLimit:2,
  mustRankBefore:[{higher:"docs/architecture.md", lower:"docs/current-context.md"}]
}]' "$fixture/profiles/fixture.json" >"$profiles/rank-lower-absent.json"
rank_lower_out="$work_root/rank-lower-out"
shin_for rank-lower-absent "$root" "$rank_lower_out" build >"$work_root/rank-lower-build.json"
shin_for rank-lower-absent "$root" "$rank_lower_out" eval --strict >"$work_root/rank-lower-eval.json"
assert_json "$work_root/rank-lower-eval.json" \
  '.ok and .cases[0].rank_assertions[0].passed and
   .cases[0].rank_assertions[0].lower_rank == null'

"$jq_bin" '.id = "rank-higher-missing" | .evalCases = [{
  id:"higher-missing", query:"result contract", mustTopN:3, limit:6, perDocLimit:2,
  mustRankBefore:[{higher:"docs/current-context.md", lower:"docs/architecture.md"}]
}]' "$fixture/profiles/fixture.json" >"$profiles/rank-higher-missing.json"
rank_higher_out="$work_root/rank-higher-out"
shin_for rank-higher-missing "$root" "$rank_higher_out" build >"$work_root/rank-higher-build.json"
expect_failure "$work_root/rank-higher-eval.json" \
  shin_for rank-higher-missing "$root" "$rank_higher_out" eval --strict
assert_json "$work_root/rank-higher-eval.json" \
  '(.ok | not) and .cases[0].rank_violations[0].reason == "higher-missing"'

"$jq_bin" '.id = "rank-invalid" | .evalCases = [{
  id:"invalid-rank", query:"result contract", mustNotPaths:["docs/architecture.md"],
  mustRankBefore:[
    {higher:"docs/architecture.md", lower:"docs/mixed-authority.md"},
    {higher:"docs/mixed-authority.md", lower:"docs/architecture.md"},
    {higher:"docs/architecture.md", lower:"docs/mixed-authority.md"}
  ]
}]' "$fixture/profiles/fixture.json" >"$profiles/rank-invalid.json"
expect_failure "$work_root/rank-invalid-profile.json" \
  shin_for rank-invalid "$root" "$work_root/rank-invalid-out" profile-check --strict
assert_contains "$work_root/rank-invalid-profile.json" 'cyclic-eval-rank-constraints'
assert_contains "$work_root/rank-invalid-profile.json" 'duplicate-eval-rank-pair'
assert_contains "$work_root/rank-invalid-profile.json" 'contradictory-eval-rank-pair'

expect_failure "$work_root/eval-limit-override.json" shin eval --strict --limit 2
assert_contains "$work_root/eval-limit-override.json" '"code": "invalid-eval-configuration"'

shin_pack_diff "$root_two" --from "$out" --to "$out_two" \
  --input "$work_root/task-request.json" --jobs 4 --require-current --strict \
  >"$work_root/pack-diff-identical.json"
shin_pack_diff "$root_two" --from "$out" --to "$out_two" \
  --input "$work_root/task-request.json" --jobs 4 --require-current --strict \
  >"$work_root/pack-diff-identical-repeat.json"
diff "$work_root/pack-diff-identical.json" \
  "$work_root/pack-diff-identical-repeat.json"
assert_json "$work_root/pack-diff-identical.json" \
  '.schema_version == "shin.agent-context-pack-diff.v1" and .ok and
   .execution.snapshot_integrity_verified and
   .comparison.same_snapshot_id and (.comparison.snapshot_changed | not) and
   (.comparison.agent_context_changed | not) and
   .comparison.target_complete and
   .evidence.added == 0 and .evidence.removed == 0 and
   .requirements.required_regressions == 0 and
   .from.source_freshness == "not-assessed" and
   .to.source_freshness == "current"'

expect_failure "$work_root/pack-diff-incomplete.json" \
  shin_pack_diff "$root_two" --from "$out" --to "$out_two" \
    --input "$work_root/task-request-incomplete.json" --strict
assert_json "$work_root/pack-diff-incomplete.json" \
  '(.ok | not) and (.comparison.target_complete | not) and
   .from.integrity_status == "verified" and .to.integrity_status == "verified"'

"$tool" snapshot-diff --from "$out" --to "$out_two" --strict \
  >"$work_root/diff-identical.json"
"$tool" snapshot-diff --from "$out" --to "$out_two" --strict \
  >"$work_root/diff-identical-repeat.json"
diff "$work_root/diff-identical.json" "$work_root/diff-identical-repeat.json"
assert_json "$work_root/diff-identical.json" \
  '.ok and .identical and .same_snapshot_id and .compatible and
   .details_returned == 0 and .details_omitted == 0 and
   .summary.syntactic_records_changed == 0'

"$tool" snapshot-diff --from "$out" --to "$out/." --strict \
  >"$work_root/diff-same-root-alias.json"
assert_json "$work_root/diff-same-root-alias.json" '.identical and .from.out_root == .to.out_root'

printf '\nThe current capability policy is version two.\n' >>"$root_two/docs/architecture.md"
printf '%s\n' '# Added Context' '' 'A newly published fixture document.' >"$root_two/docs/added-context.md"
rm "$root_two/docs/current-handoff.md"
changed_out="$work_root/out-changed"
shin_for fixture "$root_two" "$changed_out" build >"$work_root/build-changed.json"
shin_pack_diff "$root_two" --from "$out" --to "$changed_out" \
  --input "$work_root/task-request.json" --jobs 4 --limit 2 \
  --require-current --strict >"$work_root/pack-diff-changed.json"
assert_json "$work_root/pack-diff-changed.json" \
  '.ok and .comparison.snapshot_changed and
   (.comparison.agent_context_changed | not) and
   (.comparison.evidence_set_changed | not) and .comparison.receipt_changed and
   .comparison.target_complete and (.comparison.required_regression | not) and
   .requirements.required_regressions == 0 and
   .evidence.added == 0 and .evidence.removed == 0 and
   .evidence.rank_changed == 0 and .evidence.match_edges_changed == 0 and
   (.interpretation.semantic_change_claim | not)'

awk '{
  print
  if ($0 ~ /The capability result envelope contains/) {
    print "The result contract now carries a stable audit identifier."
  }
}' "$root_two/docs/architecture.md" >"$work_root/architecture-task-change.md"
mv "$work_root/architecture-task-change.md" "$root_two/docs/architecture.md"
task_changed_out="$work_root/out-task-changed"
shin_for fixture "$root_two" "$task_changed_out" build \
  >"$work_root/build-task-changed.json"
shin_pack_diff "$root_two" --from "$changed_out" --to "$task_changed_out" \
  --input "$work_root/task-request.json" --jobs 4 --limit 1 \
  --require-current --strict >"$work_root/pack-diff-task-changed.json"
assert_json "$work_root/pack-diff-task-changed.json" \
  '.ok and .comparison.snapshot_changed and .comparison.agent_context_changed and
   .comparison.evidence_set_changed and .comparison.target_complete and
   (.comparison.required_regression | not) and
   (.evidence.added + .evidence.removed + .evidence.rank_changed +
    .evidence.match_edges_changed > 0) and
   .evidence.details_returned == 1 and .evidence.details_omitted > 0 and
   (.interpretation.semantic_change_claim | not)'
"$tool" snapshot-diff --from "$out" --to "$changed_out" --limit 2 \
  >"$work_root/diff-changed.json"
assert_json "$work_root/diff-changed.json" \
  '(.ok | not) and (.identical | not) and .summary.sources_added >= 1 and
   .summary.sources_removed >= 1 and .summary.sources_changed >= 1 and
   .details_returned <= 2 and .details_omitted > 0 and
   (.summary.syntactic_records_added + .summary.syntactic_records_removed +
    .summary.syntactic_records_changed > 0) and
   (.syntactic_record_identity.semantic_change_claim | not)'
expect_failure "$work_root/diff-changed-strict.json" \
  "$tool" snapshot-diff --from "$out" --to "$changed_out" --strict
assert_json "$work_root/diff-changed-strict.json" '.strict and (.identical | not) and (.ok | not)'

"$tool" snapshot-diff --from "$out" --to "$changed_out" >"$work_root/diff-forward.json" &
forward_pid=$!
"$tool" snapshot-diff --from "$changed_out" --to "$out" >"$work_root/diff-reverse.json" &
reverse_pid=$!
wait "$forward_pid"
wait "$reverse_pid"
assert_json "$work_root/diff-forward.json" '(.identical | not)'
assert_json "$work_root/diff-reverse.json" '(.identical | not)'

corrupt_out="$work_root/out-corrupt"
cp -R "$out" "$corrupt_out"
printf '%s\n' '{"tampered":true}' >>"$corrupt_out/docs/kb/generated/documents.jsonl"
expect_failure "$work_root/diff-corrupt.json" \
  "$tool" snapshot-diff --from "$out" --to "$corrupt_out"
assert_contains "$work_root/diff-corrupt.json" '"code": "snapshot-integrity-failed"'
expect_failure "$work_root/pack-diff-corrupt.json" \
  shin_pack_diff "$root" --from "$out" --to "$corrupt_out" \
    --input "$work_root/task-request.json"
assert_contains "$work_root/pack-diff-corrupt.json" \
  '"code": "snapshot-integrity-failed"'

bad_schema_out="$work_root/out-bad-schema"
cp -R "$out" "$bad_schema_out"
"$jq_bin" '.schema_version = "unsupported"' \
  "$bad_schema_out/docs/kb/generated/manifest.json" \
  >"$bad_schema_out/docs/kb/generated/manifest.json.tmp"
mv "$bad_schema_out/docs/kb/generated/manifest.json.tmp" \
  "$bad_schema_out/docs/kb/generated/manifest.json"
expect_failure "$work_root/diff-bad-schema.json" \
  "$tool" snapshot-diff --from "$out" --to "$bad_schema_out"
assert_contains "$work_root/diff-bad-schema.json" '"code": "snapshot-manifest-invalid"'

missing_integrity_out="$work_root/out-missing-integrity"
cp -R "$out" "$missing_integrity_out"
"$jq_bin" '.generated_file_integrity.files |= .[1:]' \
  "$missing_integrity_out/docs/kb/generated/manifest.json" \
  >"$missing_integrity_out/docs/kb/generated/manifest.json.tmp"
mv "$missing_integrity_out/docs/kb/generated/manifest.json.tmp" \
  "$missing_integrity_out/docs/kb/generated/manifest.json"
expect_failure "$work_root/diff-missing-integrity.json" \
  "$tool" snapshot-diff --from "$out" --to "$missing_integrity_out"
assert_contains "$work_root/diff-missing-integrity.json" 'snapshot-integrity-bijection-failed'

orphan_output_out="$work_root/out-orphan-output"
cp -R "$out" "$orphan_output_out"
printf '%s\n' 'not declared by the published manifest' \
  >"$orphan_output_out/docs/kb/generated/orphan.txt"
expect_failure "$work_root/diff-orphan-output.json" \
  "$tool" snapshot-diff --from "$out" --to "$orphan_output_out"
assert_contains "$work_root/diff-orphan-output.json" 'snapshot-integrity-bijection-failed'

duplicate_output_out="$work_root/out-duplicate-output"
cp -R "$out" "$duplicate_output_out"
"$jq_bin" '.outputs += [.outputs[0]]' \
  "$duplicate_output_out/docs/kb/generated/manifest.json" \
  >"$duplicate_output_out/docs/kb/generated/manifest.json.tmp"
mv "$duplicate_output_out/docs/kb/generated/manifest.json.tmp" \
  "$duplicate_output_out/docs/kb/generated/manifest.json"
expect_failure "$work_root/diff-duplicate-output.json" \
  "$tool" snapshot-diff --from "$out" --to "$duplicate_output_out"
assert_contains "$work_root/diff-duplicate-output.json" '"code": "snapshot-manifest-invalid"'

symlink_layout_out="$work_root/out-symlink-layout"
cp -R "$out" "$symlink_layout_out"
mv "$symlink_layout_out/docs/kb/generated" "$symlink_layout_out/docs/kb/generated-real"
ln -s generated-real "$symlink_layout_out/docs/kb/generated"
expect_failure "$work_root/diff-symlink-layout.json" \
  "$tool" snapshot-diff --from "$out" --to "$symlink_layout_out"
assert_contains "$work_root/diff-symlink-layout.json" '"code": "snapshot-layout-unsafe"'

echo "Hermetic operability tests passed."
