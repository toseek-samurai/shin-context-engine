#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_index_fault_tests.sh <shin-context> <source-root>" >&2
  exit 2
fi

tool="$1"
source_root="$2"
fixture="$source_root/tests/fixtures/basic"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-index-faults.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
healthy_out="$work_root/out-healthy"
cp -R "$fixture/root" "$root"

shin_for() {
  local command_out="$1"
  shift
  "$tool" --profile fixture --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$command_out" "$@"
}

expect_failure_code() {
  local output="$1"
  local code="$2"
  shift 2
  if "$@" >"$output" 2>&1; then
    echo "fault-injected generated index command unexpectedly succeeded: $*" >&2
    cat "$output" >&2
    exit 1
  fi
  if ! grep -Fq "\"code\": \"$code\"" "$output"; then
    echo "expected $code in $output" >&2
    cat "$output" >&2
    exit 1
  fi
}

expect_index_corrupt() {
  local output="$1"
  shift
  expect_failure_code "$output" index-corrupt "$@"
}

write_oversized_offset_line() {
  local path="$1"
  {
    dd if=/dev/zero bs=1048576 count=1 2>/dev/null
    printf 'x'
  } | tr '\000' 'x' >"$path"
}

shin_for "$healthy_out" build >"$work_root/build.json"
shin_for "$healthy_out" check >"$work_root/check.json"

# Preserve the legacy-reader corruption suite after exercising the current writer.
python3 "$source_root/tests/fixtures/compact_lexical.py" "$healthy_out/docs/kb/generated"
shin_for "$healthy_out" check >"$work_root/legacy-check.json"

binary_offset_out="$work_root/out-binary-offset"
cp -R "$healthy_out" "$binary_offset_out"
awk -F '\t' 'BEGIN {OFS="\t"} {$3="9223372036854775807"; print}' \
  "$binary_offset_out/docs/kb/generated/terms.index.tsv" \
  >"$binary_offset_out/docs/kb/generated/terms.index.tsv.tmp"
mv "$binary_offset_out/docs/kb/generated/terms.index.tsv.tmp" \
  "$binary_offset_out/docs/kb/generated/terms.index.tsv"
expect_index_corrupt "$work_root/binary-offset.json" \
  shin_for "$binary_offset_out" search capability

sequential_offset_out="$work_root/out-sequential-offset"
cp -R "$healthy_out" "$sequential_offset_out"
awk -F '\t' 'BEGIN {OFS="\t"} {$3="9223372036854775807"; print}' \
  "$sequential_offset_out/docs/kb/generated/terms.index.tsv" \
  >"$sequential_offset_out/docs/kb/generated/terms.index.tsv.tmp"
mv "$sequential_offset_out/docs/kb/generated/terms.index.tsv.tmp" \
  "$sequential_offset_out/docs/kb/generated/terms.index.tsv"
expect_index_corrupt "$work_root/sequential-offset.json" \
  shin_for "$sequential_offset_out" search \
    "application page section resource sql query javascript write"

binary_offset_line_out="$work_root/out-binary-offset-line"
cp -R "$healthy_out" "$binary_offset_line_out"
write_oversized_offset_line \
  "$binary_offset_line_out/docs/kb/generated/terms.index.tsv"
expect_index_corrupt "$work_root/binary-offset-line.json" \
  shin_for "$binary_offset_line_out" search capability

sequential_offset_line_out="$work_root/out-sequential-offset-line"
cp -R "$healthy_out" "$sequential_offset_line_out"
write_oversized_offset_line \
  "$sequential_offset_line_out/docs/kb/generated/terms.index.tsv"
expect_index_corrupt "$work_root/sequential-offset-line.json" \
  shin_for "$sequential_offset_line_out" search \
    "application page section resource sql query javascript write"

row_offset_out="$work_root/out-row-offset"
cp -R "$healthy_out" "$row_offset_out"
awk -F '\t' 'BEGIN {OFS="\t"} {$3="9223372036854775807"; print}' \
  "$row_offset_out/docs/kb/generated/sections.index.tsv" \
  >"$row_offset_out/docs/kb/generated/sections.index.tsv.tmp"
mv "$row_offset_out/docs/kb/generated/sections.index.tsv.tmp" \
  "$row_offset_out/docs/kb/generated/sections.index.tsv"
expect_index_corrupt "$work_root/row-offset.json" \
  shin_for "$row_offset_out" context "capability result contract"

row_offset_line_out="$work_root/out-row-offset-line"
cp -R "$healthy_out" "$row_offset_line_out"
write_oversized_offset_line \
  "$row_offset_line_out/docs/kb/generated/sections.index.tsv"
expect_index_corrupt "$work_root/row-offset-line.json" \
  shin_for "$row_offset_line_out" context "capability result contract"

diagnose_offset_line_out="$work_root/out-diagnose-offset-line"
cp -R "$healthy_out" "$diagnose_offset_line_out"
write_oversized_offset_line \
  "$diagnose_offset_line_out/docs/kb/generated/sections.index.tsv"
expect_index_corrupt "$work_root/diagnose-offset-line.json" \
  shin_for "$diagnose_offset_line_out" diagnose "triggerWidget arrow-function" \
    --expected-path src/widget.ts

row_json_out="$work_root/out-row-json"
cp -R "$healthy_out" "$row_json_out"
awk '{sub(/^\{/, "["); print}' "$row_json_out/docs/kb/generated/sections.jsonl" \
  >"$row_json_out/docs/kb/generated/sections.jsonl.tmp"
mv "$row_json_out/docs/kb/generated/sections.jsonl.tmp" \
  "$row_json_out/docs/kb/generated/sections.jsonl"
expect_index_corrupt "$work_root/row-json.json" \
  shin_for "$row_json_out" context "capability result contract"

posting_out="$work_root/out-posting"
cp -R "$healthy_out" "$posting_out"
posting_file="$posting_out/docs/kb/generated/terms.jsonl"
posting_term="$(awk -F '"' '/"sections":\[\]/{print $4; exit}' "$posting_file")"
if [[ -z "$posting_term" ]]; then
  echo "fixture has no term posting with an empty sections array" >&2
  exit 1
fi
awk -v wanted="$posting_term" '
  index($0, "\"term\":\"" wanted "\"") == 2 {sub(/"sections":\[\]/, "\"sections\":{}")} {print}
' "$posting_file" >"$posting_file.tmp"
mv "$posting_file.tmp" "$posting_file"
expect_index_corrupt "$work_root/posting-field.json" \
  shin_for "$posting_out" search "$posting_term"

missing_evidence_out="$work_root/out-missing-evidence"
cp -R "$healthy_out" "$missing_evidence_out"
awk '{gsub(/"evidence_kind"/, "\"evidence_king\""); print}' \
  "$missing_evidence_out/docs/kb/generated/sections.jsonl" \
  >"$missing_evidence_out/docs/kb/generated/sections.jsonl.tmp"
mv "$missing_evidence_out/docs/kb/generated/sections.jsonl.tmp" \
  "$missing_evidence_out/docs/kb/generated/sections.jsonl"
expect_index_corrupt "$work_root/missing-evidence.json" \
  shin_for "$missing_evidence_out" context "capability result contract"

wrong_type_verification_out="$work_root/out-wrong-type-verification"
cp -R "$healthy_out" "$wrong_type_verification_out"
awk '{gsub(/"verification_level":"declared"/, "\"verification_level\":1234567890"); print}' \
  "$wrong_type_verification_out/docs/kb/generated/sections.jsonl" \
  >"$wrong_type_verification_out/docs/kb/generated/sections.jsonl.tmp"
mv "$wrong_type_verification_out/docs/kb/generated/sections.jsonl.tmp" \
  "$wrong_type_verification_out/docs/kb/generated/sections.jsonl"
expect_index_corrupt "$work_root/wrong-type-verification.json" \
  shin_for "$wrong_type_verification_out" context "capability result contract"

unknown_evidence_out="$work_root/out-unknown-evidence"
cp -R "$healthy_out" "$unknown_evidence_out"
awk '{gsub(/"evidence_kind":"architecture-document"/, "\"evidence_kind\":\"unrecognized-document\""); print}' \
  "$unknown_evidence_out/docs/kb/generated/sections.jsonl" \
  >"$unknown_evidence_out/docs/kb/generated/sections.jsonl.tmp"
mv "$unknown_evidence_out/docs/kb/generated/sections.jsonl.tmp" \
  "$unknown_evidence_out/docs/kb/generated/sections.jsonl"
expect_index_corrupt "$work_root/unknown-evidence.json" \
  shin_for "$unknown_evidence_out" context "capability result contract"

missing_identifier_terms_out="$work_root/out-missing-identifier-terms"
cp -R "$healthy_out" "$missing_identifier_terms_out"
awk '{gsub(/"identifier_terms"/, "\"identifier_term\""); print}' \
  "$missing_identifier_terms_out/docs/kb/generated/facts.jsonl" \
  >"$missing_identifier_terms_out/docs/kb/generated/facts.jsonl.tmp"
mv "$missing_identifier_terms_out/docs/kb/generated/facts.jsonl.tmp" \
  "$missing_identifier_terms_out/docs/kb/generated/facts.jsonl"
expect_index_corrupt "$work_root/missing-identifier-terms.json" \
  shin_for "$missing_identifier_terms_out" search assistantDefault

invalid_fact_range_out="$work_root/out-invalid-fact-range"
cp -R "$healthy_out" "$invalid_fact_range_out"
awk '{gsub(/"start_line":6,"end_line":9/, "\"start_line\":10,\"end_line\":9"); print}' \
  "$invalid_fact_range_out/docs/kb/generated/facts.jsonl" \
  >"$invalid_fact_range_out/docs/kb/generated/facts.jsonl.tmp"
mv "$invalid_fact_range_out/docs/kb/generated/facts.jsonl.tmp" \
  "$invalid_fact_range_out/docs/kb/generated/facts.jsonl"
expect_index_corrupt "$work_root/invalid-fact-range.json" \
  shin_for "$invalid_fact_range_out" search assistantDefault

generator_mismatch_out="$work_root/out-generator-mismatch"
cp -R "$healthy_out" "$generator_mismatch_out"
sed -E 's/(shin-generator-v[0-9]+-[a-z-]+)/\1-invalid/' \
  "$generator_mismatch_out/docs/kb/generated/manifest.json" \
  >"$generator_mismatch_out/docs/kb/generated/manifest.json.tmp"
mv "$generator_mismatch_out/docs/kb/generated/manifest.json.tmp" \
  "$generator_mismatch_out/docs/kb/generated/manifest.json"
expect_failure_code "$work_root/generator-mismatch.json" index-generator-unsupported \
  shin_for "$generator_mismatch_out" search capability

echo "Hermetic generated-index fault tests passed."
