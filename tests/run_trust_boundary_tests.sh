#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_trust_boundary_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi
jq_bin="$1"
tool="$2"
source_root="$3"
fixture="$source_root/tests/fixtures/basic"
work="$(mktemp -d "${TMPDIR:-/tmp}/shin-trust-boundary.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cp -R "$fixture/root" "$work/root"
out="$work/index"

shin() {
  "$tool" --profile fixture --profile-dir "$fixture/profiles" \
    --root "$work/root" --out-dir "$out" "$@"
}

expect_failure() {
  if "$@" >"$work/result.json" 2>&1; then
    echo "unexpected success: $*" >&2
    exit 1
  fi
  "$jq_bin" -e '.ok == false' "$work/result.json" >/dev/null
}

for command in check validate worktree-diff; do
  expect_failure shin "$command"
  [[ ! -e "$out" ]] || { echo "$command created output state" >&2; exit 1; }
done
expect_failure shin ready --strict
"$jq_bin" -e '.ready == false and .next_action == "rebuild-index"' "$work/result.json" >/dev/null
[[ ! -e "$out" ]]
expect_failure "$tool" --profile fixture --profile-dir "$fixture/profiles" \
  --root "$work/root" check
diff -qr "$fixture/root" "$work/root" >/dev/null

shin build >"$work/build.json"
cache="$out/docs/kb/.shin-context-freshness.json"
cache_lock="$out/docs/kb/.shin-context-freshness.lock"
[[ -f "$cache" && -f "$cache_lock" ]]
cp -R "$out" "$work/before"
shin brief capability --projection compact >"$work/fast.json"
"$jq_bin" -e '.index_freshness.current and .index_freshness.cache_hit' "$work/fast.json" >/dev/null
shin brief capability --projection compact --require-current >"$work/exact.json"
shin check >"$work/check.json"
shin worktree-diff --strict >"$work/diff.json"
diff -qr "$work/before" "$out" >/dev/null

rm "$cache" "$cache_lock"
shin brief capability --projection compact >"$work/uncached.json"
"$jq_bin" -e '.index_freshness.current and (.index_freshness.cache_hit | not)' "$work/uncached.json" >/dev/null
[[ ! -e "$cache" && ! -e "$cache_lock" ]]
shin build >"$work/rebuild.json"
[[ -f "$cache" && -f "$cache_lock" ]]

printf '%s\n' '{"unexpected":"fixture"}' >"$out/docs/kb/generated/orphan.jsonl"
for command in check worktree-diff; do
  expect_failure shin "$command"
  "$jq_bin" -e '.error.code == "snapshot-integrity-bijection-failed"' "$work/result.json" >/dev/null
done
expect_failure shin brief capability --require-current
"$jq_bin" -e '.error.code == "snapshot-integrity-bijection-failed"' "$work/result.json" >/dev/null
expect_failure shin brief capability
"$jq_bin" -e '.error.code == "snapshot-integrity-bijection-failed"' "$work/result.json" >/dev/null

shin build >"$work/repair.json"
rm "$out/docs/kb/generated/sections.index.tsv"
expect_failure shin brief capability --require-current
"$jq_bin" -e '.error.code == "index-unavailable"' "$work/result.json" >/dev/null
expect_failure shin brief capability
"$jq_bin" -e '.error.code == "index-unavailable"' "$work/result.json" >/dev/null
shin --source-scan brief capability --require-current >"$work/explicit-live.json"
"$jq_bin" -e '.index_mode == "source-scan" and .snapshot_id == null' "$work/explicit-live.json" >/dev/null
shin build >"$work/partial-repair.json"
mv "$out/docs/kb/generated/sections.jsonl" "$work/sections.jsonl"
ln -s "$work/sections.jsonl" "$out/docs/kb/generated/sections.jsonl"
for command in check worktree-diff; do
  expect_failure shin "$command"
  "$jq_bin" -e '.error.code | IN("snapshot-file-invalid", "snapshot-layout-unsafe")' "$work/result.json" >/dev/null
done
expect_failure shin brief capability --require-current
"$jq_bin" -e '.error.code == "snapshot-layout-unsafe"' "$work/result.json" >/dev/null
expect_failure shin brief capability
"$jq_bin" -e '.error.code == "snapshot-layout-unsafe"' "$work/result.json" >/dev/null
diff -qr "$fixture/root" "$work/root" >/dev/null
echo "Trust boundary regressions passed"
