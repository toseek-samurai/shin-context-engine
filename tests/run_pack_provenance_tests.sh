#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_pack_provenance_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
fixture="$source_root/tests/fixtures/pack-provenance"
profile_dir="$source_root/tests/fixtures/basic/profiles"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-pack-provenance.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
out="$work_root/out"
cp -R "$fixture/root" "$root"

shin() {
  "$tool" --profile fixture --profile-dir "$profile_dir" --root "$root" --out-dir "$out" "$@"
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

expect_failure "$work_root/one-record.json" \
  shin pack --input "$fixture/request-one-record.json" --strict
assert_json "$work_root/one-record.json" \
  '(.ok | not) and (.completion.complete | not) and
   .budget.returned_records == 1 and .budget.eligible_records == 2 and
   .completion.unsatisfied_required_requirement_ids == ["section-heading"] and
   ([.evidence[] | select(.evidence_type == "contract" and
                           .record.title == "Document Result Policy" and
                           .location == {path:"docs/provenance-contract.md",start_line:1,end_line:1} and
                           ([.matches[].requirement_id] == ["document-title"]) and
                           ([.matches[] | select(.matched_fields | index("title"))] | length == 1))] | length) == 1'

shin pack --input "$fixture/request-two-records.json" --strict \
  >"$work_root/two-records.json"
assert_json "$work_root/two-records.json" \
  '.ok and .completion.complete and .completion.coverage_complete and
   .execution.deduplication_policy == "canonical-record-v1" and
   .budget.eligible_records == 2 and .budget.returned_records == 2 and
   (.budget | has("pre_dedup_eligible_records") | not) and
   (.budget | has("overlap_deduplicated_records") | not) and
   ([.evidence[] | .matches | length == 1] | all) and
   ([.evidence[] | select(.evidence_type == "contract" and
                           .record.title == "Document Result Policy" and
                           .location == {path:"docs/provenance-contract.md",start_line:1,end_line:1} and
                           ([.matches[].requirement_id] == ["document-title"]) and
                           ([.matches[] | select(.matched_fields | index("title"))] | length == 1))] | length) == 1 and
   ([.evidence[] | select(.evidence_type == "section" and
                           .record.heading == "Document Result Policy" and
                           .record.row_type == "section" and
                           .location == {path:"docs/provenance-contract.md",start_line:1,end_line:2} and
                           ([.matches[].requirement_id] == ["section-heading"]) and
                           ([.matches[] | select((.matched_fields | index("heading")) and
                                                  (.matched_fields | index("row_type")))] | length == 1))] | length) == 1 and
   .verification_paths == [{path:"docs/provenance-contract.md",
                            ranges:[{start_line:1,end_line:1},{start_line:1,end_line:2}],
                            evidence_ids:.verification_paths[0].evidence_ids}] and
   (.verification_paths[0].evidence_ids | length) == 2'
