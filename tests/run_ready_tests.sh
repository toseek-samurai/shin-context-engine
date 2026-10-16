#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_ready_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-ready.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

project_root="$work_root/project"
other_root="$work_root/other"
profile_dir="$work_root/profiles"
cache="$work_root/cache"
marker="docs/machine/shin-project-adoption-v1.json"
routing="$work_root/routing.json"
mkdir -p "$project_root" "$other_root" "$profile_dir" "$cache"
cp -R "$source_root/tests/fixtures/basic/root/." "$project_root/"
cp -R "$source_root/tests/fixtures/adoption/root/." "$project_root/"
cp -R "$source_root/tests/fixtures/basic/root/." "$other_root/"
cp "$source_root/tests/fixtures/basic/profiles/fixture.json" "$profile_dir/fixture.json"
"$jq_bin" --arg marker "$marker" '
  .agentStartPaths += ["AGENTS.md", $marker] |
  .agentStartPaths |= unique
' "$profile_dir/fixture.json" >"$work_root/fixture-profile.json"
mv "$work_root/fixture-profile.json" "$profile_dir/fixture.json"
"$jq_bin" '.id = "other" | .sourceLabel = "Other fixture route"' \
  "$source_root/tests/fixtures/basic/profiles/fixture.json" >"$profile_dir/other.json"
mkdir -p "$project_root/docs/kb" "$other_root/docs/kb"

"$jq_bin" -n \
  --arg profile_dir "$profile_dir" \
  --arg project_root "$project_root" \
  --arg other_root "$other_root" \
  --arg marker "$marker" '
  {
    schemaVersion: "shin.profile-routing.v1",
    id: "ready-fixture-routing",
    routes: [
      {
        id: "fixture",
        description: "Current fixture route",
        profile: "fixture",
        profileDir: $profile_dir,
        root: $project_root,
        marker: $marker,
        signals: ["fixture project", "current fixture"]
      },
      {
        id: "other",
        description: "Other fixture route",
        profile: "other",
        profileDir: $profile_dir,
        root: $other_root,
        signals: ["other project", "other fixture"]
      }
    ]
  }' >"$routing"

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

ready() {
  "$tool" --profile fixture --profile-dir "$profile_dir" \
    --root "$project_root" --out-dir "$cache/fixture" ready "$@"
}

"$tool" --profile fixture --profile-dir "$profile_dir" \
  --root "$project_root" --out-dir "$cache/fixture" build >/dev/null

ready --strict >"$work_root/ready-known.json"
assert_json "$work_root/ready-known.json" '
  .schema_version == "shin.agent-readiness.v1" and .ok and .ready and
  (.verbose | not) and .next_command == null and
  .require_current and .strict and .next_action == "brief-or-pack" and
  .policy == {
    build: false,
    consumer_mutation: false,
    fail_fast: false,
    prerequisite_gated: true,
    profile_switch: false,
    snapshot_mutation: false
  } and
  .summary.attempted == 2 and .summary.passed == 2 and
  .summary.failed == 0 and .summary.skipped == 2 and
  .summary.readiness_requirements_failed == 0 and
  [.steps[].status] == ["skipped", "skipped", "passed", "passed"]'

ready --marker "$project_root/$marker" --routing-config "$routing" \
  --task "fixture project" --strict >"$work_root/ready-adopted.json"
ready --marker "$project_root/$marker" --routing-config "$routing" \
  --task "fixture project" --strict >"$work_root/ready-adopted-repeat.json"
# Gate durations are observations; all readiness decisions remain deterministic.
for result in ready-adopted ready-adopted-repeat; do
  assert_json "$work_root/$result.json" '
    all(.steps[3].result.gates[];
      if .attempted then ((.elapsed_ms | type) == "number" and .elapsed_ms >= 0)
      else .elapsed_ms == null end)'
  assert_json "$work_root/$result.json" \
    '.steps[3].result.preparation | .generation_runs >= 1 and .reuses >= 0 and
     (.verification_ms | type) == "number" and .verification_ms >= 0'
  jq '(.steps[3].result.gates |= map(del(.elapsed_ms))) |
      del(.steps[3].result.preparation.verification_ms)' \
    "$work_root/$result.json" >"$work_root/$result-semantic.json"
done
diff "$work_root/ready-adopted-semantic.json" "$work_root/ready-adopted-repeat-semantic.json"
assert_json "$work_root/ready-adopted.json" '
  .ok and .ready and .marker == "docs/machine/shin-project-adoption-v1.json" and
  .alignment.routing == {
    marker: true,
    profile: true,
    profile_dir: true,
    required: true,
    root: true,
    state: "current-route"
  } and
  .alignment.adoption == {index_key:true, profile:true, required:true} and
  .summary.attempted == 4 and .summary.passed == 4 and
  [.steps[].status] == ["passed", "passed", "passed", "passed"] and
  .steps[1].result.index.key == "fixture" and
  .steps[2].result.current and
  .steps[3].result.schema_version == "shin.validation-summary.v2"'

"$tool" --profile fixture --profile-dir "$profile_dir" \
  --root "$project_root" --out-dir "$cache/wrong-key" build >/dev/null
expect_failure "$work_root/wrong-index-key.json" \
  "$tool" --profile fixture --profile-dir "$profile_dir" \
    --root "$project_root" --out-dir "$cache/wrong-key" ready \
    --marker "$marker" --strict
assert_json "$work_root/wrong-index-key.json" '
  (.ok | not) and (.ready | not) and
  .alignment.adoption.index_key == false and
  .next_action == "repair-adoption-invocation" and
  .steps[1].status == "failed" and
  .steps[1].error.code == "ready-adoption-route-mismatch" and
  .steps[2].status == "skipped" and .steps[3].status == "skipped" and
  .summary.readiness_requirements_failed == 1'

missing_index="$cache/route-mismatch-must-not-open"
expect_failure "$work_root/route-mismatch.json" \
  "$tool" --profile fixture --profile-dir "$profile_dir" \
    --root "$project_root" --out-dir "$missing_index" ready \
    --routing-config "$routing" --task "other project"
assert_json "$work_root/route-mismatch.json" '
  (.ok | not) and (.ready | not) and
  .alignment.routing.state == "recommended" and
  .summary.attempted == 1 and .summary.passed == 1 and
  .summary.failed == 0 and .summary.skipped == 3 and
  .summary.readiness_requirements_failed == 1 and
  .next_action == "resolve-profile-route" and
  [.steps[].status] == ["passed", "skipped", "skipped", "skipped"]'
if [[ -e "$missing_index" ]]; then
  echo "route mismatch unexpectedly opened or mutated the selected index" >&2
  exit 1
fi

expect_failure "$work_root/route-no-match.json" \
  ready --routing-config "$routing" --task "unrelated question"
assert_json "$work_root/route-no-match.json" '
  (.ok | not) and .alignment.routing.state == "no-match" and
  .next_action == "resolve-profile-route"'

printf '\nUnpublished readiness drift.\n' >>"$project_root/docs/architecture.md"
expect_failure "$work_root/drift.json" ready
assert_json "$work_root/drift.json" '
  (.ok | not) and (.ready | not) and
  .summary.worktree_current == false and
  .next_action == "rebuild-index" and
  .next_command.command == "build" and
  .next_command.automatic_execution == false and
  .next_command.mutation == {consumer:false, index:true} and
  (.next_command.argv | index("--profile")) != null and
  (.next_command.argv | index("fixture")) != null and
  (.next_command.argv | index("build")) != null and
  .steps[2].status == "failed" and
  .steps[2].result.summary.documents_changed == 1 and
  .steps[3].status == "failed" and
  (.steps[3].result.gates | not) and
  (.diagnostic_ids | length) == 2'

expect_failure "$work_root/drift-verbose.json" ready --verbose
assert_json "$work_root/drift-verbose.json" '
  (.ok | not) and .verbose and
  .next_action == "rebuild-index" and
  .steps[3].result.schema_version == "shin.validation.v1" and
  (.steps[3].result.gates | type) == "array"'

expect_failure "$work_root/incomplete-routing.json" ready --task fixture
assert_json "$work_root/incomplete-routing.json" '
  .error.code == "incomplete-routing-request" and .error.category == "usage"'
expect_failure "$work_root/unknown-option.json" ready --unknown
assert_json "$work_root/unknown-option.json" '
  .error.code == "unsupported-option" and .error.category == "usage"'

echo "Hermetic agent readiness tests passed."
