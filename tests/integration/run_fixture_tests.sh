#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_fixture_tests.sh <shin-context> <source-root>" >&2
  exit 2
fi

tool="$1"
source_root="$2"
fixture="$source_root/tests/fixtures/basic"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-context-fixture.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
root_two="$work_root/root-two"
out="$work_root/out"
out_two="$work_root/out-two"
out_second="$work_root/out-second"
out_relative_profile="$work_root/out-relative-profile"
freshness_cache="$out/docs/kb/.shin-context-freshness.json"
freshness_lock="$out/docs/kb/.shin-context-freshness.lock"
cp -R "$fixture/root" "$root"
cp -R "$fixture/root" "$root_two"
mkdir -p "$root/docs/kb/.SHIN-CONTEXT-STAGE.fixture"
printf '%s\n' '{"internal":true}' >"$root/docs/kb/.shin-context-freshness.json"
printf '%s\n' '{"internal":true}' >"$root/docs/kb/.SHIN-CONTEXT-FIXTURE.JSON"
printf '%s\n' '{"internal":true}' >"$root/docs/kb/.SHIN-CONTEXT-STAGE.fixture/partial.json"
cp -R "$fixture/profiles" "$work_root/profiles"
cp "$fixture/federation.json" "$work_root/federation.json"
cp "$fixture/federation-reversed.json" "$work_root/federation-reversed.json"
cp "$fixture/federation-eval.json" "$work_root/federation-eval.json"
cp "$fixture/federation-eval-failing.json" "$work_root/federation-eval-failing.json"
cp "$fixture/federation-eval-unknown-member.json" "$work_root/federation-eval-unknown-member.json"
cp "$fixture/federation-eval-invalid.json" "$work_root/federation-eval-invalid.json"

shin() {
  "$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --out-dir "$out" "$@"
}

shin_for() {
  local command_root="$1"
  local command_out="$2"
  shift 2
  "$tool" --profile fixture --profile-dir "$fixture/profiles" \
    --root "$command_root" --out-dir "$command_out" "$@"
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

assert_not_contains() {
  local file="$1"
  local unexpected="$2"
  if grep -Fq "$unexpected" "$file"; then
    echo "did not expect '$unexpected' in $file" >&2
    cat "$file" >&2
    exit 1
  fi
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

snapshot_id_from() {
  local file="$1"
  local snapshot_id
  snapshot_id="$(sed -nE 's/.*"snapshot_id"[[:space:]]*:[[:space:]]*"(fnv1a64:[0-9a-f]+)".*/\1/p' "$file" | head -n 1)"
  if [[ -z "$snapshot_id" ]]; then
    echo "missing snapshot identity in $file" >&2
    cat "$file" >&2
    exit 1
  fi
  printf '%s\n' "$snapshot_id"
}

expected_version="$(tr -d '\r\n' <"$source_root/VERSION")"
if [[ "$expected_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+-(dev|rc)\.[0-9]+$ ]]; then
  expected_stability="development"
else
  expected_stability="stable"
fi
"$tool" --version >"$work_root/version.json"
"$tool" version >"$work_root/version-command.json"
diff "$work_root/version.json" "$work_root/version-command.json"
assert_contains "$work_root/version.json" '"schema_version": "shin.version.v1"'
assert_contains "$work_root/version.json" '"command": "version"'
assert_contains "$work_root/version.json" "\"version\": \"$expected_version\""
assert_contains "$work_root/version.json" "\"stability\": \"$expected_stability\""

shin profile-check --strict >"$work_root/profile-check.json"
assert_contains "$work_root/profile-check.json" '"ok": true'
assert_contains "$work_root/profile-check.json" '"command": "profile-check"'
assert_contains "$work_root/profile-check.json" '"document_exclude_prefixes":1'
assert_contains "$work_root/profile-check.json" '"json_key_include_prefixes":1'
assert_contains "$work_root/profile-check.json" '"secret_scan_exclude_prefixes":1'
assert_contains "$work_root/profile-check.json" '"source_fact_extractors":2'
assert_contains "$work_root/profile-check.json" '"source_fact_attribute_keys":5'

expect_failure "$work_root/profile-check-invalid-policy.json" \
  "$tool" --profile invalid-policy --profile-dir "$fixture/profiles" --root "$root" profile-check
assert_contains "$work_root/profile-check-invalid-policy.json" 'root-file-excluded-by-document-policy'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-profile-prefix'
assert_contains "$work_root/profile-check-invalid-policy.json" 'negative-structured-data-limit'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-source-fact-extractor'
assert_contains "$work_root/profile-check-invalid-policy.json" 'unknown-source-fact-extractor'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-source-fact-attribute-key'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-source-fact-attribute-key'
assert_contains "$work_root/profile-check-invalid-policy.json" 'source-fact-attribute-key-too-long'
assert_contains "$work_root/profile-check-invalid-policy.json" 'source-fact-attribute-keys-require-retool'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-root-file-candidate'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-optional-root-file'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-root-file'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-source-fact-root'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-source-fact-root'
assert_contains "$work_root/profile-check-invalid-policy.json" 'empty-compound-query-term'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-compound-query-term'
assert_contains "$work_root/profile-check-invalid-policy.json" 'too-many-eval-query-variants'
assert_contains "$work_root/profile-check-invalid-policy.json" 'duplicate-eval-query-variant'
assert_contains "$work_root/profile-check-invalid-policy.json" 'empty-eval-query-variant'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-eval-match-ceiling'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-eval-field'
assert_contains "$work_root/profile-check-invalid-policy.json" 'queryVariants must be an array of strings'
assert_contains "$work_root/profile-check-invalid-policy.json" 'maxCandidateRows must be an integer'
assert_contains "$work_root/profile-check-invalid-policy.json" 'minCoveredGroups must be an integer'
assert_contains "$work_root/profile-check-invalid-policy.json" 'unknown field: unexpectedGate'
assert_contains "$work_root/profile-check-invalid-policy.json" 'mustRankBefore[0] has unknown field: unexpected'
assert_contains "$work_root/profile-check-invalid-policy.json" 'mustRankBefore[0].higher must be a string'
assert_contains "$work_root/profile-check-invalid-policy.json" 'queryVariants must use only one supported field alias'
assert_contains "$work_root/profile-check-invalid-policy.json" 'query_variants must be an array of strings'
assert_contains "$work_root/profile-check-invalid-policy.json" 'maxCandidateRows is outside the supported integer range'
assert_contains "$work_root/profile-check-invalid-policy.json" 'query must be a non-empty string'
assert_contains "$work_root/profile-check-invalid-policy.json" 'must define at least one retrieval assertion'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-pack-eval-field'
assert_contains "$work_root/profile-check-invalid-policy.json" 'packEvalCases[0] has unknown field: unexpectedGate'
assert_contains "$work_root/profile-check-invalid-policy.json" 'packEvalCases[0].request must be an object'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-pack-eval-request'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-pack-eval-limit'
assert_contains "$work_root/profile-check-invalid-policy.json" 'contradictory-pack-eval-path'
assert_contains "$work_root/profile-check-invalid-policy.json" 'invalid-pack-eval-forbidden-path'
expect_failure "$work_root/eval-invalid-policy.json" \
  "$tool" --profile invalid-policy --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$out" eval
assert_contains "$work_root/eval-invalid-policy.json" '"code": "invalid-eval-configuration"'

expect_failure "$work_root/profile-check-excessive-source-fact-attributes.json" \
  "$tool" --profile excessive-source-fact-attributes --profile-dir "$fixture/profiles" \
  --root "$root" profile-check
assert_contains "$work_root/profile-check-excessive-source-fact-attributes.json" \
  'too-many-source-fact-attribute-keys'

expect_failure "$work_root/profile-check-invalid-source-fact-attribute-shape.json" \
  "$tool" --profile invalid-source-fact-attribute-shape --profile-dir "$fixture/profiles" \
  --root "$root" profile-check
assert_contains "$work_root/profile-check-invalid-source-fact-attribute-shape.json" \
  'sourceFactAttributeKeys must use only one supported field alias'
assert_contains "$work_root/profile-check-invalid-source-fact-attribute-shape.json" \
  'sourceFactAttributeKeys must be an array of strings'

runtime_group_query=""
for index in $(seq 1 65); do
  runtime_group_query+="groupterm${index} "
done
{
  printf '%s\n' '{' '  "schemaVersion": "shin.profile.v1",' \
    '  "id": "invalid-eval-runtime-groups",' \
    '  "sourceLabel": "Runtime eval group bound fixture",' \
    '  "rootFiles": ["README.md"],' '  "evalCases": [{' \
    '    "id": "too-many-runtime-groups",'
  printf '    "query": "%s",\n' "${runtime_group_query% }"
  printf '%s\n' '    "mustPaths": ["README.md"]' '  }]' '}'
} >"$work_root/profiles/invalid-eval-runtime-groups.json"
expect_failure "$work_root/eval-runtime-groups.json" \
  "$tool" --profile invalid-eval-runtime-groups --profile-dir "$work_root/profiles" \
    --root "$root" --out-dir "$out" eval --strict
assert_contains "$work_root/eval-runtime-groups.json" '"code": "invalid-eval-configuration"'

{
  printf '%s\n' '{' '  "schemaVersion": "shin.profile.v1",' \
    '  "id": "invalid-eval-runtime-expansion",' \
    '  "sourceLabel": "Runtime eval expansion bound fixture",' \
    '  "rootFiles": ["README.md"],' '  "aliases": [{' \
    '    "term": "expandme",' '    "expands": ['
  for index in $(seq 1 513); do
    separator=','
    if [[ "$index" -eq 513 ]]; then
      separator=''
    fi
    printf '      "expandedterm%s"%s\n' "$index" "$separator"
  done
  printf '%s\n' '    ]' '  }],' '  "evalCases": [{' \
    '    "id": "too-many-runtime-expanded-terms",' \
    '    "query": "expandme",' '    "mustPaths": ["README.md"]' '  }]' '}'
} >"$work_root/profiles/invalid-eval-runtime-expansion.json"
expect_failure "$work_root/eval-runtime-expansion.json" \
  "$tool" --profile invalid-eval-runtime-expansion --profile-dir "$work_root/profiles" \
    --root "$root" --out-dir "$out" eval --strict
assert_contains "$work_root/eval-runtime-expansion.json" '"code": "invalid-eval-configuration"'

expect_failure "$work_root/profile-check-empty-extractors.json" \
  "$tool" --profile empty-extractors --profile-dir "$fixture/profiles" --root "$root" profile-check
assert_contains "$work_root/profile-check-empty-extractors.json" 'source-fact-extractors-missing'

expect_failure "$work_root/profile-check-no-inputs.json" \
  "$tool" --profile no-inputs --profile-dir "$fixture/profiles" --root "$root" profile-check
assert_contains "$work_root/profile-check-no-inputs.json" 'profile-inputs-missing'

symlink_parent_root="$work_root/symlink-parent-root"
mkdir -p "$symlink_parent_root/src"
cp "$root/README.md" "$symlink_parent_root/README.md"
printf '%s\n' 'export const escaped = true;' >"$symlink_parent_root/src/escaped.ts"
ln -s "$symlink_parent_root" "$root/linked"
expect_failure "$work_root/profile-check-symlink-parent.json" \
  "$tool" --profile symlink-parent --profile-dir "$fixture/profiles" --root "$root" profile-check
assert_contains "$work_root/profile-check-symlink-parent.json" 'unsafe-optional-root-file'
assert_contains "$work_root/profile-check-symlink-parent.json" 'unsafe-source-fact-root'
assert_contains "$work_root/profile-check-symlink-parent.json" 'unsafe-agent-start-path'
rm "$root/linked"

"$tool" --root "$root" discover >"$work_root/discover.json"
assert_contains "$work_root/discover.json" '"command": "discover"'
assert_contains "$work_root/discover.json" '"ok": true'
assert_contains "$work_root/discover.json" '"has_dotnet_solution": true'
assert_contains "$work_root/discover.json" '"dotnet"'
assert_contains "$work_root/discover.json" '"csharp-source-facts"'
assert_contains "$work_root/discover.json" '"csproj_files":1'
assert_contains "$work_root/discover.json" '"csharp_files":1'
assert_contains "$work_root/discover.json" '"has_retool_source_roots": true'
assert_contains "$work_root/discover.json" '"rsx_files":4'
assert_contains "$work_root/discover.json" '"retool_workflow_files":2'
assert_contains "$work_root/discover.json" '"retool_resource_files":1'
assert_contains "$work_root/discover.json" '"generic-retool"'
assert_contains "$work_root/discover.json" '"retool-source-facts"'

shin build >"$work_root/build.txt"
"$tool" --profile fixture --profile-dir "$work_root/profiles" \
  --root "$root_two" --out-dir "$out_two" build >"$work_root/build-two.txt"
shin check >"$work_root/check.txt"
assert_contains "$work_root/build.txt" '"schema_version": "shin.build.v1"'
assert_contains "$work_root/build.txt" '"command": "build"'
assert_contains "$work_root/build.txt" '"ok": true'
assert_contains "$work_root/build.txt" '"published": true'
assert_contains "$work_root/build.txt" '"jsonl_candidate_rows": 5'
assert_contains "$work_root/build.txt" '"jsonl_indexed_rows": 4'
assert_contains "$work_root/build.txt" '"jsonl_capped_rows": 1'
assert_not_contains "$out/docs/kb/generated/documents.jsonl" 'docs/kb/.shin-context'
assert_not_contains "$out/docs/kb/generated/documents.jsonl" 'docs/kb/.SHIN-CONTEXT'
assert_not_contains "$out/docs/kb/generated/facts.jsonl" 'docs/kb/.SHIN-CONTEXT'
diff -qr "$out/docs/kb/generated" "$out_two/docs/kb/generated" \
  >"$work_root/internal-control-determinism.txt"
assert_contains "$out/docs/kb/generated/sections.jsonl" '"heading":"capabilityId"'
assert_contains "$out/docs/kb/generated/sections.jsonl" '"heading":"method"'
assert_contains "$work_root/check.txt" '"schema_version": "shin.check.v1"'
assert_contains "$work_root/check.txt" '"command": "check"'
assert_contains "$work_root/check.txt" '"current": true'
shin doctor --strict >"$work_root/doctor.json"
shin vacuum --strict >"$work_root/vacuum.json"
shin eval --strict >"$work_root/eval.json"
shin pack-eval --strict >"$work_root/pack-eval.json"
assert_contains "$work_root/pack-eval.json" '"failed": 0'
assert_contains "$work_root/pack-eval.json" '"passed": 3'
assert_contains "$work_root/pack-eval.json" '"total": 3'
expect_failure "$work_root/eval-duplicate-failed-only.json" \
  shin eval --failed-only --failed-only
assert_contains "$work_root/eval-duplicate-failed-only.json" '"code": "duplicate-option"'
expect_failure "$work_root/pack-eval-duplicate-failed-only.json" \
  shin pack-eval --failed-only --failed-only
assert_contains "$work_root/pack-eval-duplicate-failed-only.json" '"code": "duplicate-option"'
assert_contains "$work_root/doctor.json" '"ok": true'
assert_contains "$work_root/doctor.json" '"command": "doctor"'
assert_contains "$work_root/doctor.json" '"duplicate_fact_ids":0'
assert_contains "$work_root/doctor.json" '"jsonl_candidate_rows":5'
assert_contains "$work_root/doctor.json" '"jsonl_indexed_rows":4'
assert_contains "$work_root/doctor.json" '"jsonl_capped_rows":1'
assert_contains "$work_root/doctor.json" '"json_key_candidate_sections":7'
assert_contains "$work_root/doctor.json" '"json_key_indexed_sections":4'
assert_contains "$work_root/doctor.json" '"json_key_capped_sections":3'
assert_contains "$out/docs/kb/generated/manifest.json" '"json_key_include_prefixes":["docs/widget-schema.json"]'
assert_contains "$work_root/doctor.json" '"secret_scan_excluded_files":1'
assert_contains "$work_root/doctor.json" 'json-key-limit-applied'
assert_contains "$work_root/doctor.json" 'jsonl-row-limit-applied'
assert_contains "$work_root/doctor.json" 'secret-scan-path-policy-applied'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'Widget runtime event completed'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'shared-event-line-2'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'deps=Query23'
assert_contains "$out/docs/kb/generated/documents.jsonl" '"path":"docs/mixed-authority.md","kind":"markdown","doc_type":"reference","area":"docs","status":"active"'
assert_contains "$out/docs/kb/generated/documents.jsonl" '"path":"docs/current-context.md","kind":"markdown","doc_type":"context","area":"docs","status":"active"'
assert_contains "$out/docs/kb/generated/documents.jsonl" '"path":"docs/current-handoff.md","kind":"markdown","doc_type":"handoff","area":"docs","status":"active"'
assert_contains "$out/docs/kb/generated/contracts.jsonl" '"path":"docs/mixed-authority.md","line":3,"start_line":3,"end_line":3,"status":"active"'
assert_not_contains "$out/docs/kb/generated/documents.jsonl" 'docs/excluded/ignored.jsonl'
assert_not_contains "$out/docs/kb/generated/facts.jsonl" 'Widget runtime event discarded'
assert_contains "$work_root/vacuum.json" '"ok": true'
assert_contains "$work_root/vacuum.json" '"command": "vacuum"'
assert_contains "$work_root/eval.json" '"total":22,"passed":22,"failed":0'
assert_contains "$work_root/eval.json" '"query_summary": {"total":27,"passed":27,"failed":0}'
assert_contains "$work_root/eval.json" '"variant_index":0'
assert_contains "$work_root/eval.json" 'result contract capability'
assert_contains "$work_root/eval.json" '"min_covered_groups":3'
assert_contains "$work_root/eval.json" '"schema_version":"shin.query-group-coverage.v1"'
assert_contains "$work_root/eval.json" '"covered_groups":3'
assert_contains "$work_root/eval.json" '"returned_evidence_covered_groups":3'
assert_contains "$work_root/eval.json" '"must_rank_before":[{"higher":"docs/architecture.md","lower":"docs/mixed-authority.md"}]'
assert_contains "$work_root/eval.json" '"ranking_policy":"global-evidence-order-unique-path-v1"'
# Source metadata and excerpts supply exact lexical evidence, not prefix completion.
jq -e '
  . as $suite |
  [(.cases[] | ., .query_variants[]?) | select(.rank_metrics.relevant_paths > 0)] as $queries |
  (([$queries[].rank_metrics.reciprocal_rank] | add) / ($queries | length)) as $expected |
  (($suite.rank_summary.mean_reciprocal_rank - $expected | fabs) < 0.000001) and
  any($queries[]; .id == "fixture-compound-identifier-shortlist-completion" and
      .rank_metrics.first_relevant_rank == 1 and
      .returned_paths[0] == "src/ActorWorkerBridge.ts")
' "$work_root/eval.json" >/dev/null
assert_contains "$work_root/eval.json" '"command": "eval"'
published_snapshot_id="$(snapshot_id_from "$work_root/eval.json")"
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool event RetoolLoadWidget success trigger RetoolSaveWidget datasource'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool call RetoolSaveWidget trigger RetoolLoadWidget'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool call RetoolLoadWidget trigger RetoolRefresh'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'disabled={{ current_user == null }} readonlyMode=always'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'hidden={{ false }} label=Fixture Name formDataKey=name'
jq -e 'select(.path == "apps/admin/Fixture/src/Home.rsx" and
              .record_type == "retool-component" and
              .title == "retool component RetoolForm Form Home" and
              .start_line == 36 and .end_line == 40)' \
  "$out/docs/kb/generated/facts.jsonl" >/dev/null
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool resource FixtureApi restapi fixture-resource'
jq -e 'select(.path == "apps/admin/Fixture/src/Home.rsx" and
              .record_type == "retool-resource-binding" and
              .title == "retool resource-binding RetoolLoadWidget RESTQuery uses FixtureApi GET /widgets" and
              .identifier_terms == ["RESTQuery"])' \
  "$out/docs/kb/generated/facts.jsonl" >/dev/null
for mode in indexed source-scan; do
  if [[ "$mode" == source-scan ]]; then
    shin --source-scan brief "RESTQuery FixtureApi resource binding" --require-current \
      >"$work_root/retool-resource-binding-$mode.json"
  else
    shin brief "RESTQuery FixtureApi resource binding" --require-current \
      >"$work_root/retool-resource-binding-$mode.json"
  fi
  jq -e 'any(.facts[]; .record_type == "retool-resource-binding" and
                       .path == "apps/admin/Fixture/src/Home.rsx" and
                       .title == "retool resource-binding RetoolLoadWidget RESTQuery uses FixtureApi GET /widgets" and
                       (.matched_group_indices | index(0)) != null)' \
    "$work_root/retool-resource-binding-$mode.json" >/dev/null
done
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool workflow-block RetoolWorkflowLoad fixture-workflow-load RESTQuery FixtureApi workflows/FixtureWorkflow'
assert_contains "$out/docs/kb/generated/facts.jsonl" 'retool workflow-edge RetoolWorkflowStart fixture-start to RetoolWorkflowLoad workflows/FixtureWorkflow'
assert_not_contains "$out/docs/kb/generated/facts.jsonl" 'retool workflow-edge WrongWorkflowStart fixture-start to RetoolWorkflowLoad workflows/FixtureWorkflow'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"path":"src/widget.ts"'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"identifier_terms":["triggerWidget","request","WidgetRequest","Promise","assistantDefault","capabilityId"]'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"start_line":6,"end_line":9'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"evidence_kind":"implementation-source","verification_level":"source-observed"'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"path":"src/__tests__/WidgetCatalog.test.ts"'
assert_contains "$out/docs/kb/generated/facts.jsonl" '"evidence_kind":"test-source","verification_level":"source-observed"'
for store in documents sections facts decisions contracts runbooks; do
  assert_contains "$out/docs/kb/generated/$store.jsonl" '"evidence_kind"'
  assert_contains "$out/docs/kb/generated/$store.jsonl" '"verification_level"'
done
assert_contains "$out/docs/kb/generated/documents.jsonl" '"evidence_kind":"architecture-document","verification_level":"declared"'
assert_contains "$out/docs/kb/generated/decisions.jsonl" '"evidence_kind":"decision-record","verification_level":"declared"'
assert_contains "$out/docs/kb/generated/contracts.jsonl" '"evidence_kind":"machine-contract","verification_level":"declared"'
assert_contains "$out/docs/kb/generated/runbooks.jsonl" '"evidence_kind":"operational-document","verification_level":"declared"'

for identifier_query in assistantDefault assistant_default assistant-default "assistant default"; do
  shin search "$identifier_query" --require-current >"$work_root/search-${identifier_query// /-}.json"
  assert_contains "$work_root/search-${identifier_query// /-}.json" '"fallback_used": false'
  assert_contains "$work_root/search-${identifier_query// /-}.json" '"path": "src/widget.ts"'
  assert_contains "$work_root/search-${identifier_query// /-}.json" '"record_type": "ts-arrow-function"'
  assert_contains "$work_root/search-${identifier_query// /-}.json" '"start_line": 6'
  assert_contains "$work_root/search-${identifier_query// /-}.json" '"end_line": 9'
done
shin brief assistantDefault --projection compact --require-current >"$work_root/identifier-brief.json"
assert_contains "$work_root/identifier-brief.json" '"schema_version": "shin.context-brief-compact.v1"'
assert_contains "$work_root/identifier-brief.json" '"verification_paths"'
assert_contains "$work_root/identifier-brief.json" '"path":"src/widget.ts"'
assert_contains "$work_root/identifier-brief.json" '"start_line":6'
assert_contains "$work_root/identifier-brief.json" '"end_line":9'

"$tool" federated-brief --config "$work_root/federation.json" \
  "overview capability contract rollback deployment widget workflow resource security" \
  --limit 6 --per-doc 1 --require-current >"$work_root/federated-brief.json"
assert_contains "$work_root/federated-brief.json" '"schema_version": "shin.context-federated-brief.v1"'
assert_contains "$work_root/federated-brief.json" '"command": "federated-brief"'
assert_contains "$work_root/federated-brief.json" '"snapshot_consistency": "cooperating-local-index-set-v1"'
assert_contains "$work_root/federated-brief.json" '"member_count": 2'
assert_contains "$work_root/federated-brief.json" '"deduplicated_records": 3'
assert_contains "$work_root/federated-brief.json" 'docs/architecture.md'
assert_contains "$work_root/federated-brief.json" 'docs/operations.md'
assert_contains "$work_root/federated-brief.json" 'workflows/FixtureWorkflow/RetoolWorkflowLoad.yml'
assert_contains "$work_root/federated-brief.json" '"evidence_kind"'
assert_contains "$work_root/federated-brief.json" '"verification_level"'

"$tool" federated-brief --config "$work_root/federation.json" \
  "current-handoff exact owner policy integration readiness release evidence" \
  --limit 4 --per-doc 1 --require-current >"$work_root/federated-owner.json"
assert_contains "$work_root/federated-owner.json" '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/federated-owner.json" '"document_owner_scope_used": true'
assert_contains "$work_root/federated-owner.json" '"matching_policy": "document-owner-scope-v1"'

"$tool" federated-brief --config "$work_root/federation-reversed.json" \
  "overview capability contract rollback deployment widget workflow resource security" \
  --limit 6 --per-doc 1 --require-current >"$work_root/federated-brief-reversed.json"
sed -E 's#"config": "[^"]+"#"config": "<config>"#' \
  "$work_root/federated-brief.json" >"$work_root/federated-brief-normalized.json"
sed -E 's#"config": "[^"]+"#"config": "<config>"#' \
  "$work_root/federated-brief-reversed.json" >"$work_root/federated-brief-reversed-normalized.json"
diff "$work_root/federated-brief-normalized.json" \
  "$work_root/federated-brief-reversed-normalized.json"

expect_failure "$work_root/federated-global-profile-error.json" \
  "$tool" --profile fixture federated-brief --config "$work_root/federation.json" fixture
assert_contains "$work_root/federated-global-profile-error.json" '"code": "unsupported-option"'

"$tool" federated-eval --config "$work_root/federation.json" \
  --suite "$work_root/federation-eval.json" --strict >"$work_root/federated-eval.json"
assert_contains "$work_root/federated-eval.json" '"schema_version": "shin.federated-eval.v1"'
assert_contains "$work_root/federated-eval.json" '"command": "federated-eval"'
assert_contains "$work_root/federated-eval.json" '"stable_snapshot_set": true'
assert_contains "$work_root/federated-eval.json" '"total": 1'
assert_contains "$work_root/federated-eval.json" '"passed": 1'
assert_contains "$work_root/federated-eval.json" '"failed": 0'

expect_failure "$work_root/federated-eval-failing-strict.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval-failing.json" --strict
assert_contains "$work_root/federated-eval-failing-strict.json" '"ok": false'
assert_contains "$work_root/federated-eval-failing-strict.json" 'docs/does-not-exist.md'
expect_failure "$work_root/federated-eval-failing-soft.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval-failing.json"
assert_contains "$work_root/federated-eval-failing-soft.json" '"ok": false'

expect_failure "$work_root/federated-eval-unknown-member.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval-unknown-member.json" --strict
assert_contains "$work_root/federated-eval-unknown-member.json" '"code": "invalid-federation-eval"'

expect_failure "$work_root/federated-eval-invalid.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval-invalid.json" --strict
assert_contains "$work_root/federated-eval-invalid.json" '"code": "invalid-federation-eval"'
assert_contains "$work_root/federated-eval-invalid.json" 'unknown field: unexpectedPolicy'

printf 'interrupted-fixture\n' >"$out_two/docs/kb/.shin-context-publishing"
expect_failure "$work_root/federated-publication-incomplete.json" \
  "$tool" federated-brief --config "$work_root/federation.json" fixture
assert_contains "$work_root/federated-publication-incomplete.json" '"code": "publication-incomplete"'
expect_failure "$work_root/federated-eval-publication-incomplete.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval.json" --strict
assert_contains "$work_root/federated-eval-publication-incomplete.json" '"code": "publication-incomplete"'
rm "$out_two/docs/kb/.shin-context-publishing"

printf '\nReplica source drift.\n' >>"$root_two/README.md"
"$tool" federated-brief --config "$work_root/federation.json" fixture \
  >"$work_root/federated-stale.json"
assert_contains "$work_root/federated-stale.json" '"current": false'
expect_failure "$work_root/federated-stale-current.json" \
  "$tool" federated-brief --config "$work_root/federation.json" fixture --require-current
assert_contains "$work_root/federated-stale-current.json" '"code": "stale-index"'
expect_failure "$work_root/federated-eval-stale.json" \
  "$tool" federated-eval --config "$work_root/federation.json" \
    --suite "$work_root/federation-eval.json" --strict
assert_contains "$work_root/federated-eval-stale.json" '"freshness_violation": true'

# Readers never create cache state; only explicit publication prepares it.
rm -f "$freshness_cache"
shin search "capability result contract" --limit 3 >"$work_root/search.json"
assert_contains "$work_root/search.json" '"schema_version": "shin.search.v1"'
assert_contains "$work_root/search.json" '"command": "search"'
assert_contains "$work_root/search.json" '"ok": true'
assert_contains "$work_root/search.json" 'docs/architecture.md'
assert_contains "$work_root/search.json" '"snapshot_id": "fnv1a64:'
assert_contains "$work_root/search.json" '"query_coverage": {'
assert_contains "$work_root/search.json" '"matched_group_indices": ['
assert_contains "$work_root/search.json" '"evidence_kind": "machine-contract"'
assert_contains "$work_root/search.json" '"verification_level": "declared"'
assert_contains "$work_root/search.json" '"record_type": "contract"'
assert_contains "$work_root/search.json" '"mode": "manifest-all-source-and-output-hash"'
assert_contains "$work_root/search.json" '"cache_hit": false'
if [[ "$(snapshot_id_from "$work_root/search.json")" != "$published_snapshot_id" ]]; then
  echo "search and eval observed different published snapshots" >&2
  exit 1
fi
if [[ -f "$freshness_cache" ]]; then
  echo "read-only freshness verification published a cache" >&2
  exit 1
fi
shin build >"$work_root/cache-publication.json"

shin context "capability result contract" --limit 3 --per-doc 1 >"$work_root/context.json"
assert_contains "$work_root/context.json" '"command": "context"'
assert_contains "$work_root/context.json" '"ok": true'
assert_contains "$work_root/context.json" '"schema_version":"shin.query-group-coverage.v1"'
assert_contains "$work_root/context.json" '"covered_groups":3'
assert_contains "$work_root/context.json" '"matched_group_indices":[0,1,2]'
assert_contains "$work_root/context.json" '"evidence_kind":"architecture-document"'
assert_contains "$work_root/context.json" '"verification_level":"declared"'
assert_contains "$work_root/context.json" '"mode":"verified-metadata-cache"'
assert_contains "$work_root/context.json" '"cache_hit":true'
if [[ "$(snapshot_id_from "$work_root/context.json")" != "$published_snapshot_id" ]]; then
  echo "context and eval observed different published snapshots" >&2
  exit 1
fi

shin bench "capability result contract" --iterations 1 --build-iterations 1 --warmup 0 \
  >"$work_root/bench.json"
assert_contains "$work_root/bench.json" '"command": "bench"'
assert_contains "$work_root/bench.json" '"ok": true'
assert_contains "$work_root/bench.json" '"build_memory_scope":"process-high-water-mark-at-phase-boundary"'
assert_contains "$work_root/bench.json" '"build_phase_peak_rss_bytes":{'
assert_contains "$work_root/bench.json" '"term_worker_maps":'
assert_contains "$work_root/bench.json" '"term_merged":'
assert_contains "$work_root/bench.json" '"render_complete":'

"$tool" help >"$work_root/help.json"
assert_contains "$work_root/help.json" '"schema_version": "shin.help.v1"'
assert_contains "$work_root/help.json" '"command": "help"'
assert_contains "$work_root/help.json" '[--projection full|compact]'

expect_failure "$work_root/error-missing-command.json" "$tool"
assert_contains "$work_root/error-missing-command.json" '"code": "missing-command"'
assert_contains "$work_root/error-missing-command.json" '"command": "unknown"'

expect_failure "$work_root/error-unknown-command.json" "$tool" unsupported-command
assert_contains "$work_root/error-unknown-command.json" '"code": "unknown-command"'
assert_contains "$work_root/error-unknown-command.json" '"exit_code": 2'

expect_failure "$work_root/error-invalid-limit.json" shin search fixture --limit invalid
assert_contains "$work_root/error-invalid-limit.json" '"schema_version": "shin.error.v1"'
assert_contains "$work_root/error-invalid-limit.json" '"command": "search"'
assert_contains "$work_root/error-invalid-limit.json" '"code": "invalid-option-value"'
assert_contains "$work_root/error-invalid-limit.json" '"exit_code": 2'

expect_failure "$work_root/error-unknown-profile.json" \
  "$tool" --profile missing --profile-dir "$fixture/profiles" brief fixture
assert_contains "$work_root/error-unknown-profile.json" '"code": "unknown-profile"'
assert_contains "$work_root/error-unknown-profile.json" '"category": "configuration"'

shin brief "capability result contract" --limit 6 --per-doc 2 >"$work_root/brief.json"
assert_contains "$work_root/brief.json" '"index_mode": "generated-index"'
assert_contains "$work_root/brief.json" '"command": "brief"'
assert_contains "$work_root/brief.json" '"ok": true'
assert_contains "$work_root/brief.json" 'docs/architecture.md'
assert_contains "$work_root/brief.json" '"mode":"verified-metadata-cache"'
assert_contains "$work_root/brief.json" '"cache_hit":true'
assert_contains "$work_root/brief.json" '"schema_version":"shin.query-group-coverage.v1"'
assert_contains "$work_root/brief.json" '"covered_groups":3'
assert_contains "$work_root/brief.json" '"projection": "full"'
if [[ "$(snapshot_id_from "$work_root/brief.json")" != "$published_snapshot_id" ]]; then
  echo "brief and eval observed different published snapshots" >&2
  exit 1
fi

shin brief "capability result contract missing-evidence" --limit 6 --per-doc 2 \
  --projection compact >"$work_root/brief-compact.json"
assert_contains "$work_root/brief-compact.json" '"schema_version": "shin.context-brief-compact.v1"'
assert_contains "$work_root/brief-compact.json" '"projection": "compact"'
assert_contains "$work_root/brief-compact.json" '"schema_version":"shin.query-group-coverage-compact.v1"'
assert_contains "$work_root/brief-compact.json" '"uncovered_groups"'
assert_contains "$work_root/brief-compact.json" '"ranked_paths"'
assert_contains "$work_root/brief-compact.json" '"evidence"'
assert_contains "$work_root/brief-compact.json" '"authority_level"'
assert_contains "$work_root/brief-compact.json" '"evidence_kind"'
assert_contains "$work_root/brief-compact.json" '"verification_level"'
assert_not_contains "$work_root/brief-compact.json" '"query_diagnostics"'
assert_not_contains "$work_root/brief-compact.json" '"score_components"'
assert_not_contains "$work_root/brief-compact.json" '"matched_sections"'
if [[ "$(snapshot_id_from "$work_root/brief-compact.json")" != "$published_snapshot_id" ]]; then
  echo "compact brief and full brief observed different published snapshots" >&2
  exit 1
fi

expect_failure "$work_root/error-invalid-brief-projection.json" \
  shin brief fixture --projection diagnostic
assert_contains "$work_root/error-invalid-brief-projection.json" '"code": "invalid-option-value"'
assert_contains "$work_root/error-invalid-brief-projection.json" '"command": "brief"'

for query_command in search context brief; do
  expect_failure "$work_root/error-unknown-${query_command}-option.json" \
    shin "$query_command" fixture --telemetry
  assert_contains "$work_root/error-unknown-${query_command}-option.json" '"code": "unsupported-option"'
  assert_contains "$work_root/error-unknown-${query_command}-option.json" "\"command\": \"${query_command}\""
done

shin brief -- --telemetry >"$work_root/brief-literal-option-query.json"
assert_contains "$work_root/brief-literal-option-query.json" '"query": "--telemetry"'

assert_contains "$out/docs/kb/generated/sections.jsonl" '"status":"active"'
assert_contains "$out/docs/kb/generated/sections.jsonl" '"authority_level":"source-of-truth"'
assert_contains "$out/docs/kb/generated/sections.jsonl" '"evidence_kind":"architecture-document","verification_level":"declared"'

shin brief fixture --limit 3 --per-doc 1 >"$work_root/brief-global-budget.json"
assert_contains "$work_root/brief-global-budget.json" '"policy":"global-record-document-diversity-with-type-seeding-v3"'
assert_contains "$work_root/brief-global-budget.json" '"max_records":3'
assert_contains "$work_root/brief-global-budget.json" '"returned_records":3'
assert_contains "$work_root/brief-global-budget.json" '"eligible_records":10'
assert_contains "$work_root/brief-global-budget.json" '"budgeted_out_records":7'
assert_contains "$work_root/brief-global-budget.json" '"returned_sections": 0'
assert_contains "$work_root/brief-global-budget.json" '"returned_facts": 3'
assert_contains "$work_root/brief-global-budget.json" '"returned_by_type": {"contracts":1,"decisions":1,"facts":0,"runbooks":1}'

shin brief "current-handoff exact owner policy integration readiness release evidence" \
  --limit 4 --per-doc 1 >"$work_root/brief-document-owner.json"
assert_contains "$work_root/brief-document-owner.json" '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-document-owner.json" '"document_owner_scope_used":true'
assert_contains "$work_root/brief-document-owner.json" '"matching_policy":"document-owner-scope-v1"'
assert_contains "$work_root/brief-document-owner.json" '"ranked_paths": [{"first_evidence_rank":1,"path":"docs/current-handoff.md","unique_path_rank":1}]'
assert_not_contains "$work_root/brief-document-owner.json" 'docs/handoff-index.md'

shin brief "a-bc exact owner policy current verification evidence" \
  --limit 4 --per-doc 1 >"$work_root/brief-owner-separator.json"
assert_contains "$work_root/brief-owner-separator.json" '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-separator.json" '"path":"docs/a-bc.md"'
assert_not_contains "$work_root/brief-owner-separator.json" 'docs/ab-c.md'

shin brief "docs/space owner.md exact owner policy current verification evidence" \
  --limit 4 --per-doc 1 >"$work_root/brief-owner-space.json"
assert_contains "$work_root/brief-owner-space.json" '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-space.json" '"path":"docs/space owner.md"'

shin brief "show distributedowner alpha beta coverage" \
  --limit 4 --per-doc 1 >"$work_root/brief-owner-distributed.json"
assert_contains "$work_root/brief-owner-distributed.json" '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-distributed.json" '"path":"docs/distributedowner.md"'
jq -e '.query_diagnostics.document_owner_scope_used and .matched_sections > 1' \
  "$work_root/brief-owner-distributed.json" >/dev/null

shin brief "Release Review floating panel folders list preview" \
  --limit 4 --per-doc 2 >"$work_root/brief-owner-humanized-identifier.json"
assert_contains "$work_root/brief-owner-humanized-identifier.json" \
  '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-humanized-identifier.json" \
  '"path":"docs/ReleaseReviewPanel.md"'
jq -e '.query_diagnostics.document_owner_scope_used and
       .query_diagnostics.candidate_rows < 48 and
       .ranked_paths[0].path == "docs/ReleaseReviewPanel.md"' \
  "$work_root/brief-owner-humanized-identifier.json" >/dev/null

shin brief "Deferred Review authorization confirmation workflow evidence before Panel decision" \
  --limit 6 --per-doc 2 >"$work_root/brief-owner-humanized-distant-query.json"
assert_contains "$work_root/brief-owner-humanized-distant-query.json" \
  'docs/0042-deferred-review-decision.md'
assert_not_contains "$work_root/brief-owner-humanized-distant-query.json" \
  '"retrieval_mode": "owner-scoped"'
jq -e '.query_diagnostics.document_owner_scope_used == false and
       .ranked_paths[0].path == "docs/0042-deferred-review-decision.md"' \
  "$work_root/brief-owner-humanized-distant-query.json" >/dev/null

shin brief "Shared Audit focused panel evidence" \
  --limit 4 --per-doc 1 >"$work_root/brief-owner-humanized-ambiguous.json"
assert_not_contains "$work_root/brief-owner-humanized-ambiguous.json" \
  '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-humanized-ambiguous.json" \
  '"document_owner_scope_used":false'

shin brief "Workflow Review web reference policy evidence" \
  --limit 4 --per-doc 1 >"$work_root/brief-owner-humanized-specific-topic.json"
assert_not_contains "$work_root/brief-owner-humanized-specific-topic.json" \
  '"retrieval_mode": "owner-scoped"'
assert_contains "$work_root/brief-owner-humanized-specific-topic.json" \
  'docs/WorkflowReviewWebReferencePolicy.md'

"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --source-scan \
  brief fixture --limit 3 --per-doc 1 >"$work_root/brief-source-global-budget.json"
assert_contains "$work_root/brief-source-global-budget.json" '"index_mode": "source-scan"'
assert_contains "$work_root/brief-source-global-budget.json" '"snapshot_id": null'
assert_contains "$work_root/brief-source-global-budget.json" '"returned_records":3'
jq -e '.evidence_budget.budgeted_out_records > 0 and
       .evidence_budget.budgeted_out_records ==
         (.evidence_budget.eligible_records - .evidence_budget.returned_records)' \
  "$work_root/brief-source-global-budget.json" >/dev/null
"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --source-scan \
  search capability >"$work_root/search-source-scan.json"
"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --source-scan \
  context "capability result contract" >"$work_root/context-source-scan.json"
"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --source-scan \
  search RetoolSaveWidget --limit 4 --require-current >"$work_root/search-source-fact-scan.json"
"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --source-scan \
  brief "RetoolSaveWidget trigger" --projection compact --limit 4 --per-doc 2 --require-current \
  >"$work_root/brief-source-fact-scan.json"
assert_contains "$work_root/search-source-scan.json" '"snapshot_id": null'
assert_contains "$work_root/context-source-scan.json" '"snapshot_id":null'
assert_contains "$work_root/search-source-scan.json" '"evidence_kind": "architecture-document"'
assert_contains "$work_root/search-source-scan.json" '"verification_level": "declared"'
assert_contains "$work_root/context-source-scan.json" '"evidence_kind":"architecture-document"'
assert_contains "$work_root/context-source-scan.json" '"verification_level":"declared"'
jq -e '.index_freshness.source_scan.source_fact_rows > 0 and
       .index_freshness.fully_verified == true and
       any(.hits[]; .record_type == "retool-call" and .verification_level == "source-observed")' \
  "$work_root/search-source-fact-scan.json" >/dev/null
assert_contains "$work_root/brief-source-fact-scan.json" 'apps/admin/Fixture/lib/RetoolSaveWidget.js'
assert_contains "$work_root/brief-source-fact-scan.json" '"verification_paths"'
assert_contains "$work_root/brief-source-fact-scan.json" '"snapshot_id": null'
"$tool" --profile capped-source-scan --profile-dir "$fixture/profiles" --root "$root" \
  --source-scan search RetoolSaveWidget >"$work_root/search-source-scan-capped.json"
jq -e '.index_freshness.current == true and
       .index_freshness.fully_verified == false and
       .index_freshness.source_scan.source_fact_capped_files > 0' \
  "$work_root/search-source-scan-capped.json" >/dev/null
expect_failure "$work_root/search-source-scan-capped-current.json" \
  "$tool" --profile capped-source-scan --profile-dir "$fixture/profiles" --root "$root" \
    --source-scan search RetoolSaveWidget --require-current
assert_contains "$work_root/search-source-scan-capped-current.json" '"code": "incomplete-source-scan"'

shin brief "current fixture platform state authoring publish operator context" \
  --limit 6 --per-doc 2 >"$work_root/brief-long-query-coverage.json"
assert_contains "$work_root/brief-long-query-coverage.json" '"retrieval_mode": "relaxed"'
assert_contains "$work_root/brief-long-query-coverage.json" '"scope_fallback_used":true'
assert_contains "$work_root/brief-long-query-coverage.json" '"matching_policy":"first-term-must-rest-should-v1"'
assert_contains "$work_root/brief-long-query-coverage.json" '"required_group_matches":1'
assert_contains "$work_root/brief-long-query-coverage.json" '"total_term_groups":8'
assert_contains "$work_root/brief-long-query-coverage.json" 'docs/current-context.md'
assert_contains "$work_root/brief-long-query-coverage.json" 'docs/current-handoff.md'

shin brief "engine bridge actor-worker host-call request result affinity routing command validation" \
  --limit 8 --per-doc 2 >"$work_root/brief-compound-identifier-completion.json"
assert_contains "$work_root/brief-compound-identifier-completion.json" \
  'src/ActorWorkerBridge.ts'
jq -e '.query_diagnostics.candidate_index_used and
       .query_diagnostics.candidate_rows > 2 and
       .query_diagnostics.candidate_rows <= 64 and
       any(.facts[]; .path == "src/ActorWorkerBridge.ts" and
           .record_type == "ts-arrow-function" and .start_line == 1 and .end_line == 17)' \
  "$work_root/brief-compound-identifier-completion.json" >/dev/null
shin search "engine bridge actor-worker host-call request result affinity routing command validation" \
  --limit 8 >"$work_root/search-compound-identifier-completion.json"
shin context "engine bridge actor-worker host-call request result affinity routing command validation" \
  --limit 8 --per-doc 2 >"$work_root/context-compound-identifier-completion.json"
assert_contains "$work_root/search-compound-identifier-completion.json" \
  'src/ActorWorkerBridge.ts'
assert_contains "$work_root/context-compound-identifier-completion.json" \
  'src/ActorWorkerBridge.ts'

shin brief "overview capability contract rollback deployment widget workflow resource security" \
  --limit 6 --per-doc 1 >"$work_root/brief-document-coverage-plan.json"
assert_contains "$work_root/brief-document-coverage-plan.json" '"retrieval_mode": "planned"'
assert_contains "$work_root/brief-document-coverage-plan.json" '"planner_attempted":true'
assert_contains "$work_root/brief-document-coverage-plan.json" '"query_planner_used":true'
assert_contains "$work_root/brief-document-coverage-plan.json" '"planner_rejection_reason":null'
assert_contains "$work_root/brief-document-coverage-plan.json" '"matching_policy":"document-coverage-planner-v1"'
assert_contains "$work_root/brief-document-coverage-plan.json" '"planner_covered_groups":7'
assert_contains "$work_root/brief-document-coverage-plan.json" '"selected_document_covered_groups":6'
jq -e '.query_coverage.groups[] | select(.term == "workflow") |
  .returned_evidence_covered == true and .selected_document_covered == false and
  (.selected_document_paths | length) == 0' "$work_root/brief-document-coverage-plan.json" >/dev/null
assert_contains "$work_root/brief-document-coverage-plan.json" '"selected_document_paths"'
assert_contains "$work_root/brief-document-coverage-plan.json" '"planner_disposition":"merged"'
assert_contains "$work_root/brief-document-coverage-plan.json" 'docs/architecture.md'
assert_contains "$work_root/brief-document-coverage-plan.json" 'docs/operations.md'
assert_contains "$work_root/brief-document-coverage-plan.json" 'workflows/FixtureWorkflow/RetoolWorkflowLoad.yml'

shin brief "unknown capability contract nebulax orbitx photonx quasarx vectrix" \
  --limit 6 --per-doc 1 >"$work_root/brief-rejected-document-plan.json"
assert_contains "$work_root/brief-rejected-document-plan.json" '"retrieval_mode": "relaxed"'
assert_contains "$work_root/brief-rejected-document-plan.json" '"planner_attempted":true'
assert_contains "$work_root/brief-rejected-document-plan.json" '"query_planner_used":false'
assert_contains "$work_root/brief-rejected-document-plan.json" '"planner_rejection_reason":"insufficient-document-coverage"'
assert_contains "$work_root/brief-rejected-document-plan.json" '"selected_document_covered_groups":0'
jq -e '.query_diagnostics.planner_covered_groups >= 2' \
  "$work_root/brief-rejected-document-plan.json" >/dev/null
assert_contains "$work_root/brief-rejected-document-plan.json" '"returned_records":0'

shin brief "apps/admin/Fixture RetoolLoadWidget" --limit 8 --per-doc 2 \
  >"$work_root/brief-mixed-posting-substring.json"
assert_contains "$work_root/brief-mixed-posting-substring.json" '"retrieval_mode": "directory-scoped"'
assert_contains "$work_root/brief-mixed-posting-substring.json" '"directory_owner_scope_used":true'
assert_contains "$work_root/brief-mixed-posting-substring.json" '"matching_policy":"directory-owner-scope-v1"'
assert_contains "$work_root/brief-mixed-posting-substring.json" 'apps/admin/Fixture/src/Home.rsx'
assert_not_contains "$work_root/brief-mixed-posting-substring.json" 'apps/admin/Fixture2/'

shin brief "apps/ad/minFixture RetoolLoadWidget" --limit 8 --per-doc 2 \
  >"$work_root/brief-directory-component-collision.json"
assert_contains "$work_root/brief-directory-component-collision.json" '"retrieval_mode": "directory-scoped"'
assert_contains "$work_root/brief-directory-component-collision.json" '"returned_records":0'
assert_not_contains "$work_root/brief-directory-component-collision.json" 'apps/admin/Fixture/'

shin search "strict health check" --limit 4 >"$work_root/search-singular-plural.json"
assert_contains "$work_root/search-singular-plural.json" 'docs/operations.md'
assert_contains "$work_root/search-singular-plural.json" '"covered_groups": 3'
assert_contains "$work_root/search-singular-plural.json" '"candidate_index_used": true'

# The generic observation has an explicit origin; its typed projection does not.
# They are not exact copies and must not discard either origin before selection.
shin brief "Canonical Fixture Decision" --limit 6 --per-doc 3 >"$work_root/brief-deduplicated.json"
assert_contains "$work_root/brief-deduplicated.json" '"deduplicated_facts": 0'
assert_contains "$work_root/brief-deduplicated.json" '"returned_by_type": {"contracts":0,"decisions":2'
assert_contains "$work_root/brief-deduplicated.json" '"source":"docs/architecture.md"'
assert_contains "$work_root/brief-deduplicated.json" '"source":"docs/data/decisions.jsonl"'
assert_contains "$work_root/brief.json" '"fully_verified":true'

shin brief "capability result contract" --limit 6 --per-doc 2 --require-current \
  >"$work_root/brief-current.json"
assert_contains "$work_root/brief-current.json" '"fully_verified":true'
assert_contains "$work_root/brief-current.json" '"mode":"full-generated-output"'

printf '{malformed-cache\n' >"$freshness_cache"
shin brief "capability result contract" >"$work_root/brief-malformed-cache.json"
assert_contains "$work_root/brief-malformed-cache.json" '"mode":"manifest-all-source-and-output-hash"'
assert_contains "$work_root/brief-malformed-cache.json" '"cache_hit":false'
assert_contains "$freshness_cache" '{malformed-cache'
shin build >"$work_root/cache-repair-publication.json"
shin brief "capability result contract" >"$work_root/brief-repaired-cache.json"
assert_contains "$work_root/brief-repaired-cache.json" '"mode":"verified-metadata-cache"'
assert_contains "$work_root/brief-repaired-cache.json" '"cache_hit":true'

printf '%s\n' \
  '{"schema_version":"shin.freshness-cache.v1",' \
  '"generator_version":"invalid","manifest_hash":"invalid","profile":"fixture",' \
  '"root":"invalid","out_root":"invalid","verification":"full-content-hash",' \
  '"profile_config_hash":"invalid","source_count":18446744073709551615,' \
  '"source_fact_candidate_files":0,"source_fact_capped_files":0,"files":[]}' \
  >"$freshness_cache"
shin brief "capability result contract" >"$work_root/brief-overflow-cache.json"
assert_contains "$work_root/brief-overflow-cache.json" '"mode":"manifest-all-source-and-output-hash"'
assert_contains "$work_root/brief-overflow-cache.json" '"cache_hit":false'

touch -t 202001010000 "$root/docs/architecture.md"
shin brief "capability result contract" >"$work_root/brief-metadata-change.json"
assert_contains "$work_root/brief-metadata-change.json" '"mode":"manifest-all-source-and-output-hash"'
assert_contains "$work_root/brief-metadata-change.json" '"cache_hit":false'
assert_contains "$work_root/brief-metadata-change.json" '"current":true'
shin build >"$work_root/metadata-cache-publication.json"
shin brief "capability result contract" >"$work_root/brief-metadata-reverified.json"
assert_contains "$work_root/brief-metadata-reverified.json" '"mode":"verified-metadata-cache"'
assert_contains "$work_root/brief-metadata-reverified.json" '"cache_hit":true'

"$tool" --profile fixture --profile-dir "$fixture/profiles" --root "$root" --out-dir "$out_second" \
  build >"$work_root/build-second.txt"
diff -qr "$out/docs/kb/generated" "$out_second/docs/kb/generated" >"$work_root/determinism.txt"

(
  cd "$source_root"
  "$tool" --profile fixture --profile-dir "tests/fixtures/basic/profiles" --root "$root" \
    --out-dir "$out_relative_profile" build >"$work_root/build-relative-profile.txt"
)
diff -qr "$out/docs/kb/generated" "$out_relative_profile/docs/kb/generated" \
  >"$work_root/profile-path-determinism.txt"
assert_contains "$out/docs/kb/generated/manifest.json" '"profile_config": "profiles/fixture.json"'
assert_not_contains "$out/docs/kb/generated/manifest.json" "$source_root"

concurrent_out="$work_root/out-concurrent"
shin_for "$root" "$concurrent_out" build >"$work_root/concurrent-one.txt" 2>&1 &
concurrent_one=$!
shin_for "$root" "$concurrent_out" build >"$work_root/concurrent-two.txt" 2>&1 &
concurrent_two=$!
concurrent_status=0
wait "$concurrent_one" || concurrent_status=1
wait "$concurrent_two" || concurrent_status=1
if [[ "$concurrent_status" -ne 0 ]]; then
  cat "$work_root/concurrent-one.txt" "$work_root/concurrent-two.txt" >&2
  exit 1
fi
shin_for "$root" "$concurrent_out" check >"$work_root/concurrent-check.txt"
shin_for "$root" "$concurrent_out" brief "capability result contract" \
  >"$work_root/concurrent-baseline.json"
concurrent_snapshot_id="$(snapshot_id_from "$work_root/concurrent-baseline.json")"

concurrent_pids=()
for iteration in 1 2 3 4; do
  shin_for "$root" "$concurrent_out" build >"$work_root/publish-$iteration.txt" 2>&1 &
  concurrent_pids+=("$!")
  for reader in 1 2 3 4; do
    shin_for "$root" "$concurrent_out" brief "capability result contract" \
      >"$work_root/reader-$iteration-$reader.json" 2>&1 &
    concurrent_pids+=("$!")
  done
done
for concurrent_pid in "${concurrent_pids[@]}"; do
  if ! wait "$concurrent_pid"; then
    echo "reader/writer concurrency command failed" >&2
    exit 1
  fi
done
for iteration in 1 2 3 4; do
  for reader in 1 2 3 4; do
    reader_output="$work_root/reader-$iteration-$reader.json"
    assert_contains "$reader_output" '"ok": true'
    assert_contains "$reader_output" '"schema_version":"shin.query-group-coverage.v1"'
    assert_contains "$reader_output" '"covered_groups":3'
    if [[ "$(snapshot_id_from "$reader_output")" != "$concurrent_snapshot_id" ]]; then
      echo "reader observed a partial or unexpected published snapshot: $reader_output" >&2
      exit 1
    fi
  done
done

concurrent_control="$concurrent_out/docs/kb"
mv "$concurrent_control/generated" "$concurrent_control/.shin-context-backup"
mkdir "$concurrent_control/.shin-context-stage.stale"
printf 'stale-stage\n' >"$concurrent_control/.shin-context-stage.stale/partial.jsonl"
printf 'interrupted-test\n' >"$concurrent_control/.shin-context-publishing"
expect_failure "$work_root/interrupted-check.txt" shin_for "$root" "$concurrent_out" check
assert_contains "$work_root/interrupted-check.txt" '"schema_version": "shin.error.v1"'
assert_contains "$work_root/interrupted-check.txt" '"code": "publication-incomplete"'
cp "$root/README.md" "$work_root/readme-before-recovery-test.md"
printf '\n[Missing during recovery](docs/missing-during-recovery.md)\n' >>"$root/README.md"
expect_failure "$work_root/recovery-with-broken-source.txt" shin_for "$root" "$concurrent_out" build
assert_contains "$work_root/recovery-with-broken-source.txt" '"code": "broken-local-links"'
assert_contains "$work_root/recovery-with-broken-source.txt" '"published": false'
if [[ ! -d "$concurrent_control/generated" ||
      -e "$concurrent_control/.shin-context-backup" ||
      -e "$concurrent_control/.shin-context-publishing" ||
      -e "$concurrent_control/.shin-context-stage.stale" ]]; then
  echo "healthy snapshot was not recovered before rejected generation" >&2
  exit 1
fi
diff -qr "$concurrent_control/generated" "$out_second/docs/kb/generated" \
  >"$work_root/recovered-snapshot-diff.txt"
cp "$work_root/readme-before-recovery-test.md" "$root/README.md"
shin_for "$root" "$concurrent_out" build >"$work_root/recovered-build.txt"
shin_for "$root" "$concurrent_out" check >"$work_root/recovered-check.txt"
shin_for "$root" "$concurrent_out" brief "capability result contract" \
  >"$work_root/recovered-brief.json"
if [[ "$(snapshot_id_from "$work_root/recovered-brief.json")" != "$concurrent_snapshot_id" ]]; then
  echo "recovery did not restore the deterministic published snapshot" >&2
  exit 1
fi
if [[ -e "$concurrent_control/.shin-context-backup" ||
      -e "$concurrent_control/.shin-context-publishing" ]]; then
  echo "interrupted publication controls were not cleaned" >&2
  exit 1
fi

broken_root="$work_root/root-broken"
broken_out="$work_root/out-broken"
cp -R "$fixture/root" "$broken_root"
shin_for "$broken_root" "$broken_out" build >"$work_root/broken-baseline.txt"
cp -R "$broken_out/docs/kb/generated" "$work_root/broken-healthy-snapshot"
printf '\n[Missing target](docs/missing-target.md)\n' >>"$broken_root/README.md"
expect_failure "$work_root/broken-build.txt" shin_for "$broken_root" "$broken_out" build
assert_contains "$work_root/broken-build.txt" '"code": "broken-local-links"'
assert_contains "$work_root/broken-build.txt" '"published": false'
diff -qr "$work_root/broken-healthy-snapshot" "$broken_out/docs/kb/generated" \
  >"$work_root/broken-snapshot-diff.txt"

symlink_out="$work_root/out-symlink"
symlink_escape="$work_root/symlink-escape"
mkdir -p "$symlink_out/docs/kb" "$symlink_escape"
ln -s "$symlink_escape" "$symlink_out/docs/kb/generated"
expect_failure "$work_root/symlink-build.txt" shin_for "$root" "$symlink_out" build
assert_contains "$work_root/symlink-build.txt" 'must not be a symlink'
if [[ -e "$symlink_escape/manifest.json" ]]; then
  echo "generated output escaped through a symlink" >&2
  exit 1
fi

lock_out="$work_root/out-lock-symlink"
lock_escape="$work_root/lock-escape.txt"
mkdir -p "$lock_out/docs/kb"
printf 'sentinel\n' >"$lock_escape"
ln -s "$lock_escape" "$lock_out/docs/kb/.shin-context-writer.lock"
expect_failure "$work_root/lock-symlink-build.txt" shin_for "$root" "$lock_out" build
assert_contains "$work_root/lock-symlink-build.txt" 'must be a regular file'
assert_contains "$lock_escape" 'sentinel'

freshness_escape="$work_root/freshness-escape.txt"
printf 'freshness-sentinel\n' >"$freshness_escape"
rm -f "$freshness_cache"
ln -s "$freshness_escape" "$freshness_cache"
expect_failure "$work_root/cache-symlink-brief.txt" shin brief "capability result contract"
assert_contains "$work_root/cache-symlink-brief.txt" 'Freshness cache must be a regular file'
assert_contains "$freshness_escape" 'freshness-sentinel'
rm -f "$freshness_cache"
shin brief "capability result contract" >"$work_root/cache-symlink-recovered.json"
assert_contains "$work_root/cache-symlink-recovered.json" '"cache_hit":false'

rm -f "$freshness_lock"
ln -s "$freshness_escape" "$freshness_lock"
expect_failure "$work_root/freshness-lock-symlink-brief.txt" shin brief "capability result contract"
assert_contains "$work_root/freshness-lock-symlink-brief.txt" 'must be a regular file'
assert_contains "$freshness_escape" 'freshness-sentinel'
rm -f "$freshness_lock"
shin build >"$work_root/freshness-lock-publication.json"
shin brief "capability result contract" >"$work_root/freshness-lock-recovered.json"
assert_contains "$work_root/freshness-lock-recovered.json" '"cache_hit":true'

printf '\n{"corrupt":true}\n' >>"$out/docs/kb/generated/facts.jsonl"
shin brief "triggerWidget arrow-function" >"$work_root/brief-corrupt-fast.json"
assert_contains "$work_root/brief-corrupt-fast.json" '"current":false'
assert_contains "$work_root/brief-corrupt-fast.json" '"fully_verified":false'
assert_contains "$work_root/brief-corrupt-fast.json" '"cache_hit":false'
assert_contains "$work_root/brief-corrupt-fast.json" '"mode":"manifest-all-source-and-output-hash"'
if [[ "$(snapshot_id_from "$work_root/brief-corrupt-fast.json")" != "$published_snapshot_id" ]]; then
  echo "output corruption changed the declared published snapshot identity" >&2
  exit 1
fi
expect_failure "$work_root/check-corrupt.txt" shin check
assert_contains "$work_root/check-corrupt.txt" '"schema_version": "shin.check.v1"'
assert_contains "$work_root/check-corrupt.txt" '"ok": false'
assert_contains "$work_root/check-corrupt.txt" 'docs/kb/generated/facts.jsonl'
expect_failure "$work_root/brief-corrupt.txt" shin brief "triggerWidget arrow-function" --require-current
assert_contains "$work_root/brief-corrupt.txt" '"code": "stale-index"'
shin vacuum --apply --strict >"$work_root/vacuum-repair.json"
assert_contains "$work_root/vacuum-repair.json" '"ok": true'
shin check >"$work_root/check-vacuum-repair.txt"
shin brief "triggerWidget arrow-function" >"$work_root/brief-vacuum-repair.json"
if [[ "$(snapshot_id_from "$work_root/brief-vacuum-repair.json")" != "$published_snapshot_id" ]]; then
  echo "canonical vacuum repair changed a deterministic snapshot identity" >&2
  exit 1
fi

printf '\nexport const fixtureRevision = 2;\n' >>"$root/src/widget.ts"
shin brief "triggerWidget arrow-function" >"$work_root/brief-source-stale.json"
assert_contains "$work_root/brief-source-stale.json" '"current":false'
assert_contains "$work_root/brief-source-stale.json" '"fully_verified":false'
assert_contains "$work_root/brief-source-stale.json" '"cache_hit":false'
if [[ "$(snapshot_id_from "$work_root/brief-source-stale.json")" != "$published_snapshot_id" ]]; then
  echo "unpublished source drift changed the active snapshot identity" >&2
  exit 1
fi
expect_failure "$work_root/brief-source-stale-current.txt" \
  shin brief "triggerWidget arrow-function" --require-current
shin build >"$work_root/build-source-revision.txt"
shin brief "triggerWidget arrow-function" >"$work_root/brief-source-revision.json"
if [[ "$(snapshot_id_from "$work_root/brief-source-revision.json")" == "$published_snapshot_id" ]]; then
  echo "content-changing publication retained the previous snapshot identity" >&2
  exit 1
fi

secret_value="sk-proj-""z9Y8x7W6v5U4t3S2r1Q0p"
printf '\nexport const fixtureApiKey = "%s";\n' "$secret_value" >>"$root/src/widget.ts"
expect_failure "$work_root/doctor-secret.json" shin doctor
assert_contains "$work_root/doctor-secret.json" 'secret-like-source-content'
assert_not_contains "$work_root/doctor-secret.json" "$secret_value"

python3 "$source_root/tests/fixtures/json-output/test_json_output.py" "$tool"

echo "Hermetic fixture tests passed."
