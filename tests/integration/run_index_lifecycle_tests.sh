#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_index_lifecycle_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-index-lifecycle.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

cache="$work_root/cache"
mkdir -p "$cache"

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

build_index() {
  local key="$1"
  "$tool" --profile fixture \
    --profile-dir "$source_root/tests/fixtures/basic/profiles" \
    --root "$source_root/tests/fixtures/basic/root" \
    --out-dir "$cache/$key" build >/dev/null
}

build_index current
cp -R "$cache/current" "$cache/unused"
cp -R "$cache/current" "$cache/legacy"
cp -R "$cache/current" "$cache/corrupt"
"$jq_bin" '.generator_version = "shin-generator-v13-evidence-provenance"' \
  "$cache/legacy/docs/kb/generated/manifest.json" >"$work_root/legacy-manifest.json"
mv "$work_root/legacy-manifest.json" "$cache/legacy/docs/kb/generated/manifest.json"
printf '\ncorrupt\n' >>"$cache/corrupt/docs/kb/generated/sections.jsonl"
mkdir -p "$cache/empty/docs/kb"
ln -s "$cache/current" "$cache/symlink-index"
printf 'unmanaged\n' >"$cache/.notes"

"$tool" index-list --cache-dir "$cache" >"$work_root/list.json"
"$tool" index-list --cache-dir "$cache" >"$work_root/list-repeat.json"
diff "$work_root/list.json" "$work_root/list-repeat.json"
assert_json "$work_root/list.json" '
  .schema_version == "shin.index-list.v1" and .ok and (.clean | not) and
  .current_generator == "shin-generator-v24-objective-cpp" and
  .policy.scope == "immediate-child-managed-indexes-v1" and
  (.policy.snapshot_mutation | not) and
  .summary == {
    corrupt: 1,
    current: 2,
    empty_shell: 1,
    entries: 7,
    incompatible_generator: 1,
    managed: 4,
    snapshot_bytes: .summary.snapshot_bytes,
    unmanaged: 1,
    unsafe: 1
  } and .summary.snapshot_bytes > 0 and
  ([.entries[] | select(.key == "legacy" and
                        .state == "incompatible-generator" and
                        .snapshot.integrity_status == "verified")] | length) == 1 and
  ([.entries[] | select(.key == "corrupt" and .state == "corrupt" and
                        .error != null)] | length) == 1 and
  ([.entries[] | select(.key == "symlink-index" and
                        .state == "unsafe-entry")] | length) == 1'

expect_failure "$work_root/list-strict.json" \
  "$tool" index-list --cache-dir "$cache" --strict
assert_json "$work_root/list-strict.json" '
  .schema_version == "shin.index-list.v1" and (.ok | not) and
  (.clean | not) and .strict'

"$tool" index-prune --cache-dir "$cache" --keep-key current \
  >"$work_root/prune-dry.json"
assert_json "$work_root/prune-dry.json" '
  .schema_version == "shin.index-prune.v1" and .ok and (.clean | not) and
  (.apply | not) and .policy.dry_run_default and
  .summary.candidates == 1 and .summary.pruned == 0 and
  .summary.remaining_candidates == 1 and
  .actions == [{
    key: "legacy",
    path: .actions[0].path,
    reason: "incompatible-generator",
    snapshot_id: .actions[0].snapshot_id,
    generator_version: "shin-generator-v13-evidence-provenance",
    snapshot_bytes: .actions[0].snapshot_bytes,
    apply_requested: false,
    applied: false,
    error: null
  }]'
test -d "$cache/legacy/docs/kb/generated"

expect_failure "$work_root/prune-dry-strict.json" \
  "$tool" index-prune --cache-dir "$cache" --keep-key current --strict
assert_json "$work_root/prune-dry-strict.json" '
  (.ok | not) and .strict and .summary.remaining_candidates == 1'

"$tool" index-prune --cache-dir "$cache" --keep-key current \
  --prune-unreferenced >"$work_root/prune-unreferenced.json"
assert_json "$work_root/prune-unreferenced.json" '
  .ok and (.clean | not) and .summary.candidates == 2 and
  [.actions[].key] == ["legacy", "unused"] and
  [.actions[].reason] == ["incompatible-generator", "unreferenced"]'

"$tool" index-prune --cache-dir "$cache" --keep-key current \
  --prune-unreferenced --apply >"$work_root/prune-apply.json"
assert_json "$work_root/prune-apply.json" '
  .ok and (.clean | not) and .apply and
  .summary.candidates == 2 and .summary.pruned == 2 and
  .summary.failures == 0 and .summary.remaining_candidates == 0 and
  .summary.bytes_reclaimed == .summary.bytes_reclaimable and
  all(.actions[]; .applied and .error == null)'
test ! -e "$cache/legacy/docs/kb/generated"
test ! -e "$cache/unused/docs/kb/generated"
test -d "$cache/current/docs/kb/generated"
test -d "$cache/corrupt/docs/kb/generated"
test -d "$cache/symlink-index/docs/kb/generated"

"$tool" index-list --cache-dir "$cache" >"$work_root/list-after.json"
assert_json "$work_root/list-after.json" '
  .summary.current == 1 and .summary.incompatible_generator == 0 and
  .summary.empty_shell == 3 and .summary.corrupt == 1 and .summary.unsafe == 1'

build_index protected
"$jq_bin" '.generator_version = "shin-generator-v13-evidence-provenance"' \
  "$cache/protected/docs/kb/generated/manifest.json" >"$work_root/protected-manifest.json"
mv "$work_root/protected-manifest.json" "$cache/protected/docs/kb/generated/manifest.json"
expect_failure "$work_root/protected.json" \
  "$tool" index-prune --cache-dir "$cache" --keep-key protected --apply --strict
assert_json "$work_root/protected.json" '
  (.ok | not) and .summary.protected_incompatible == 1 and
  .summary.candidates == 0 and .summary.pruned == 0'
test -d "$cache/protected/docs/kb/generated"

expect_failure "$work_root/missing-keep.json" \
  "$tool" index-prune --cache-dir "$cache" --keep-key missing --apply
assert_json "$work_root/missing-keep.json" '
  (.ok | not) and .missing_keep_keys == ["missing"] and
  .summary.candidates == 0 and .summary.pruned == 0 and .actions == []'
test -d "$cache/protected/docs/kb/generated"

expect_failure "$work_root/unreferenced-without-keep.json" \
  "$tool" index-prune --cache-dir "$cache" --prune-unreferenced
assert_json "$work_root/unreferenced-without-keep.json" '
  .error.code == "missing-keep-key" and .error.category == "usage"'

ln -s "$cache" "$work_root/cache-link"
expect_failure "$work_root/cache-symlink.json" \
  "$tool" index-list --cache-dir "$work_root/cache-link"
assert_json "$work_root/cache-symlink.json" '
  .error.code == "invalid-cache-root" and .error.category == "usage"'

expect_failure "$work_root/filesystem-root.json" \
  "$tool" index-list --cache-dir /
assert_json "$work_root/filesystem-root.json" '
  .error.code == "unsafe-cache-root" and .error.category == "usage"'

expect_failure "$work_root/global-root.json" \
  "$tool" --root "$cache" index-list --cache-dir "$cache"
assert_json "$work_root/global-root.json" '
  .error.code == "unsupported-option" and .error.category == "usage"'

echo "Hermetic index lifecycle tests passed."
