#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_authority_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
fixture="$source_root/tests/fixtures/authority"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-authority.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
out="$work_root/out"
cp -R "$fixture/root" "$root"

assert_row() {
  local file="$1"
  local expression="$2"
  if ! "$jq_bin" -e "$expression" "$file" >/dev/null; then
    echo "jq assertion failed for $file: $expression" >&2
    cat "$file" >&2
    exit 1
  fi
}

"$tool" --profile authority --profile-dir "$fixture/profiles" --root "$root" --out-dir "$out" build \
  >"$work_root/build.json"

assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/current-contract.md" and .status == "active")'
for decision in 1-owner 0001-openapi-owner 123456-owner; do
  assert_row "$out/docs/kb/generated/documents.jsonl" \
    "select(.path == \"docs/decisions/$decision.md\" and .doc_type == \"adr\" and .status == \"accepted\")"
  assert_row "$out/docs/kb/generated/decisions.jsonl" \
    "select(.path == \"docs/decisions/$decision.md\" and .status == \"accepted\" and .authority_level == \"accepted\")"
done
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/decisions/0001extra-owner.md" and .doc_type == "reference")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/current-contract.md" and .title == "Result Contract" and .status == "active" and .authority_level == "source-of-truth")'
assert_row "$out/docs/kb/generated/decisions.jsonl" \
  'select(.path == "docs/current-contract.md" and .title == "Accepted Decision" and .status == "active" and .authority_level == "accepted")'
assert_row "$out/docs/kb/generated/decisions.jsonl" \
  'select(.path == "docs/current-contract.md" and .title == "Unknown Decision" and .status == "pending-review" and .authority_level == "advisory")'
assert_row "$out/docs/kb/generated/decisions.jsonl" \
  'select(.path == "docs/current-contract.md" and .title == "Negated Decision" and .status == "not accepted" and .authority_level == "advisory")'

assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/local-overrides.md" and .heading == "Retired Contract" and .status == "deprecated" and .authority_level == "history")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/local-overrides.md" and .status == "active")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/local-overrides.md" and .title == "Retired Contract" and .status == "deprecated" and .authority_level == "history")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/local-overrides.md" and .heading == "Sibling Contract" and .status == "active" and .authority_level == "source-of-truth")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/local-overrides.md" and .title == "Sibling Contract" and .status == "active" and .authority_level == "source-of-truth")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/local-overrides.md" and .heading == "Fenced Contract Example" and .status == "active" and .authority_level == "source-of-truth")'

assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/archive/metadata-contract.json" and .status == "active")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/archive/metadata-contract.json" and .status == "active" and .authority_level == "history")'
assert_row "$out/docs/kb/generated/decisions.jsonl" \
  'select(.path == "docs/archive/decisions/0001-accepted-decision.md" and .title == "Archived Accepted Decision" and .status == "accepted" and .authority_level == "history")'
assert_row "$out/docs/kb/generated/decisions.jsonl" \
  'select(.path == "docs/archive/declared-decisions.jsonl" and .title == "Declared Archive Decision" and .status == "accepted" and .authority_level == "accepted")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/metadata-state-contract.json" and .status == "rejected")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/metadata-state-contract.json" and .status == "rejected" and .authority_level == "advisory")'

assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/history/legacy-contract.md" and .status == "superseded")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/history/legacy-contract.md" and .title == "Legacy Contract" and .status == "superseded" and .authority_level == "history")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/preview=review-contract.md" and .status == "active")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/preview=review-contract.md" and .title == "Preview Contract" and .status == "active" and .authority_level == "source-of-truth")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/HistoryService-contract.md" and .status == "active")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/review/review-notes.md" and .status == "review")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/review/review-notes.md" and .status == "review" and .authority_level == "review")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/reviews/review-notes.md" and .status == "review")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/reviews/review-notes.md" and .status == "review" and .authority_level == "review")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/findings/finding-notes.md" and .status == "review")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/findings/finding-notes.md" and .status == "review" and .authority_level == "review")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/recommendations/recommendation-notes.md" and .status == "review")'
assert_row "$out/docs/kb/generated/sections.jsonl" \
  'select(.path == "docs/recommendations/recommendation-notes.md" and .status == "review" and .authority_level == "review")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/proposals/draft-contract.md" and .status == "draft")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/proposals/draft-contract.md" and .title == "Draft Contract" and .status == "draft" and .authority_level == "advisory")'

assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/source-current-contract.md" and .status == "active" and .verification_level == "declared")'
assert_row "$out/docs/kb/generated/contracts.jsonl" \
  'select(.path == "docs/source-current-contract.md" and .title == "Source Current Contract" and .verification_level == "declared" and .authority_level == "source-of-truth")'
assert_row "$out/docs/kb/generated/documents.jsonl" \
  'select(.path == "docs/fenced-preamble-contract.md" and .status == "active")'
if ! "$jq_bin" -s -e '
  ([.[] | select(.path == "docs/chunked-rejected-contract.md" and .row_type == "chunk")]) as $chunks |
  ($chunks | length >= 2) and
  ($chunks | all(.[]; .status == "rejected" and .authority_level == "advisory"))
' "$out/docs/kb/generated/sections.jsonl" >/dev/null; then
  echo "rejected section chunks did not inherit lifecycle authority" >&2
  cat "$out/docs/kb/generated/sections.jsonl" >&2
  exit 1
fi
if "$jq_bin" -e 'select(.path == "docs/fenced-preamble-contract.md" and .title == "Result Contract")' \
    "$out/docs/kb/generated/contracts.jsonl" >/dev/null; then
  echo "fenced sample produced a typed contract row" >&2
  cat "$out/docs/kb/generated/contracts.jsonl" >&2
  exit 1
fi
