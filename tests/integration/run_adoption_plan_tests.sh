#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_adoption_plan_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-adoption-plan.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for adoption plan tests: $jq_bin" >&2
  exit 2
fi

profile_dir="$work_root/profiles"
mkdir -p "$profile_dir"
cp "$source_root"/profiles/generic-*.json "$profile_dir/"

node_root="$work_root/node"
cp -R "$source_root/tests/fixtures/cold-start/node" "$node_root"
cp -R "$node_root" "$work_root/node-before"
"$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
  >"$work_root/node-plan-1.json"
"$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
  >"$work_root/node-plan-2.json"
cmp "$work_root/node-plan-1.json" "$work_root/node-plan-2.json"
diff -qr "$work_root/node-before" "$node_root" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.adoption-plan.v1" and
  .command == "adoption-plan" and
  .ok == true and
  .state == "bootstrap-ready" and
  .deterministic == true and
  .project_mutation_performed == false and
  .discovery.indexable == true and
  .discovery.primary_profile == "generic-node" and
  .discovery.project_mutation_required == false and
  .marker == {
    path: "docs/machine/shin-project-adoption-v1.json",
    path_safe: true,
    present: false,
    safe_regular: false,
    selection: "canonical-proposal",
    status: "missing"
  } and
  .bootstrap.available == true and
  .bootstrap.bootstrap_only == true and
  .bootstrap.valid == true and
  .candidate_profile == null and
  .readiness == {
    adoption_check_ready: false,
    adoption_validated: false,
    bootstrap_ready: true,
    marker_authoring_ready: false
  } and
  .external_output == {
    bootstrap_key_source: "temporary-bootstrap-choice",
    consumer_writes_allowed: false,
    directory_template: "<EXTERNAL_OUTPUT_BASE>/<INDEX_KEY>",
    permanent_key_source: "marker.index.key"
  } and
  all(.commands[] | select((.argv | index("--out-dir")) != null);
      .argv[(.argv | index("--out-dir")) + 1] ==
        "<EXTERNAL_OUTPUT_BASE>/<INDEX_KEY>") and
  (.review_requirements | index("canonical-marker-index-key")) != null and
  (.review_requirements | index("single-project-adoption-gate")) != null and
  (.gaps | length) == 0 and
  .next_actions == ["run-bootstrap-route", "review-permanent-profile-policy"] and
  ([.commands[].id] | index("bootstrap-profile-check")) != null and
  ([.commands[].id] | index("bootstrap-build")) != null and
  ([.commands[].id] | index("bootstrap-brief")) != null and
  ([.commands[].id] | index("adoption-check")) != null and
  all(.commands[]; .writes_consumer == false)
' "$work_root/node-plan-1.json" >/dev/null

empty_root="$work_root/empty"
cp -R "$source_root/tests/fixtures/cold-start/empty" "$empty_root"
"$tool" --profile-dir "$profile_dir" --root "$empty_root" adoption-plan \
  >"$work_root/empty-plan.json"
"$jq_bin" -e '
  .state == "not-indexable" and
  .discovery.state == "empty" and
  .discovery.indexable == false and
  .bootstrap.profile == null and
  .readiness.bootstrap_ready == false and
  any(.gaps[]; .code == "no-indexable-content" and .stage == "bootstrap") and
  .next_actions == ["add-indexable-source-or-machine-docs"]
' "$work_root/empty-plan.json" >/dev/null

blocked_profiles="$work_root/blocked-profiles"
cp -R "$profile_dir" "$blocked_profiles"
rm "$blocked_profiles/generic-node.json"
"$tool" --profile-dir "$blocked_profiles" --root "$node_root" adoption-plan \
  >"$work_root/blocked-plan.json"
"$jq_bin" -e '
  .state == "blocked" and
  .readiness.bootstrap_ready == false and
  any(.gaps[]; .code == "bootstrap-profile-unavailable") and
  .next_actions == ["resolve-bootstrap-profile-gaps"]
' "$work_root/blocked-plan.json" >/dev/null

candidate_root="$work_root/candidate"
mkdir -p "$candidate_root"
cp -R "$source_root/tests/fixtures/basic/root/." "$candidate_root/"
cp "$source_root/tests/fixtures/adoption/root/AGENTS.md" "$candidate_root/AGENTS.md"
cp "$source_root/tests/fixtures/basic/profiles/fixture.json" "$profile_dir/fixture.json"
marker="docs/machine/shin-project-adoption-v1.json"
"$jq_bin" --arg marker "$marker" '
  .sourceFactRoots = ["src", "resources", "workflows"] |
  .agentStartPaths += ["AGENTS.md", $marker] |
  .agentStartPaths |= unique
' "$profile_dir/fixture.json" >"$work_root/fixture-profile.json"
mv "$work_root/fixture-profile.json" "$profile_dir/fixture.json"

"$tool" --profile fixture --profile-dir "$profile_dir" --root "$candidate_root" \
  adoption-plan >"$work_root/candidate-plan.json"
"$jq_bin" -e '
  .state == "marker-authoring-ready" and
  .marker.present == false and
  .agent.present == true and
  .candidate_profile.id == "fixture" and
  .candidate_profile.available == true and
  .candidate_profile.bootstrap_only == false and
  .candidate_profile.eval_cases > 0 and
  .candidate_profile.pack_eval_cases > 0 and
  .candidate_profile.marker_registered == true and
  .candidate_profile.agent_entry_registered == true and
  .candidate_profile.strict_profile_valid_now == false and
  .candidate_profile.strict_profile_valid_after_marker_creation == true and
  .candidate_profile.ready_for_marker_authoring == true and
  any(.candidate_profile.profile_check.issues[];
      .code == "missing-agent-start-path" and
      .path == "docs/machine/shin-project-adoption-v1.json" and
      .resolved_by_marker_creation_candidate == true) and
  .readiness.marker_authoring_ready == true and
  (.gaps | length) == 0 and
  .next_actions == [
    "review-and-create-adoption-marker",
    "run-adoption-check",
    "run-permanent-quality-gates"
  ]
' "$work_root/candidate-plan.json" >/dev/null

mkdir -p "$candidate_root/docs/machine"
cp "$source_root/tests/fixtures/adoption/root/$marker" "$candidate_root/$marker"
"$tool" --profile-dir "$profile_dir" --root "$candidate_root" adoption-plan \
  >"$work_root/adopted-plan.json"
"$jq_bin" -e '
  .state == "adoption-check-ready" and
  .marker.path == "docs/machine/shin-project-adoption-v1.json" and
  .marker.selection == "conventional-existing" and
  .marker.safe_regular == true and
  .readiness.adoption_check_ready == true and
  .readiness.adoption_validated == false and
  .next_actions == ["run-adoption-check"]
' "$work_root/adopted-plan.json" >/dev/null
"$tool" --profile-dir "$profile_dir" --root "$candidate_root" \
  adoption-check --marker "$marker" --strict >"$work_root/adoption-check.json"
"$jq_bin" -e '.ok == true and .profile.id == "fixture"' "$work_root/adoption-check.json" >/dev/null

rm "$candidate_root/$marker"
mkdir -p "$candidate_root/docs/architecture"
cp "$source_root/tests/fixtures/adoption/root/$marker" \
  "$candidate_root/docs/architecture/shin-context-engine-adoption.json"
"$tool" --profile-dir "$profile_dir" --root "$candidate_root" adoption-plan \
  >"$work_root/legacy-marker-plan.json"
"$jq_bin" -e '
  .state == "adoption-check-ready" and
  .marker.path == "docs/architecture/shin-context-engine-adoption.json" and
  .marker.selection == "conventional-existing"
' "$work_root/legacy-marker-plan.json" >/dev/null
rm "$candidate_root/docs/architecture/shin-context-engine-adoption.json"

"$tool" --profile generic-node --profile-dir "$profile_dir" --root "$node_root" \
  adoption-plan >"$work_root/bootstrap-candidate.json"
"$jq_bin" -e '
  .state == "promotion-incomplete" and
  .candidate_profile.bootstrap_only == true and
  .candidate_profile.eval_cases == 0 and
  .candidate_profile.pack_eval_cases == 0 and
  any(.gaps[]; .code == "bootstrap-profile-not-permanent") and
  any(.gaps[]; .code == "permanent-profile-eval-missing") and
  any(.gaps[]; .code == "permanent-profile-pack-eval-missing")
' "$work_root/bootstrap-candidate.json" >/dev/null

"$tool" --profile missing-profile --profile-dir "$profile_dir" --root "$node_root" \
  adoption-plan >"$work_root/missing-candidate.json"
"$jq_bin" -e '
  .state == "promotion-incomplete" and
  .candidate_profile.available == false and
  .candidate_profile.error.code == "unknown-profile" and
  any(.gaps[]; .code == "candidate-profile-unavailable")
' "$work_root/missing-candidate.json" >/dev/null

if "$tool" --profile ../fixture --profile-dir "$profile_dir" --root "$node_root" \
    adoption-plan >"$work_root/profile-traversal.out" 2>"$work_root/profile-traversal.err"; then
  echo "adoption-plan unexpectedly accepted profile traversal" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "invalid-profile-id" and .error.category == "usage"' \
  "$work_root/profile-traversal.err" >/dev/null

"$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
  --marker "$node_root/$marker" >"$work_root/absolute-marker-inside-root.json"
"$jq_bin" -e --arg marker "$marker" \
  '.ok and .marker.path == $marker and .marker.selection == "explicit"' \
  "$work_root/absolute-marker-inside-root.json" >/dev/null

if "$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
    --marker /tmp/adoption.json >"$work_root/absolute-marker.out" \
    2>"$work_root/absolute-marker.err"; then
  echo "adoption-plan unexpectedly accepted a marker outside --root" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "repository-file-outside-root" and
  .error.category == "usage"' \
  "$work_root/absolute-marker.err" >/dev/null

if "$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
    --marker package.json/adoption.json >"$work_root/marker-probe.out" \
    2>"$work_root/marker-probe.err"; then
  echo "adoption-plan unexpectedly treated an untraversable marker path as missing" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "marker-probe-failed" and .error.category == "configuration"' \
  "$work_root/marker-probe.err" >/dev/null

mkdir -p "$node_root/docs/machine"
ln -s "$work_root/node-plan-1.json" "$node_root/$marker"
if "$tool" --profile-dir "$profile_dir" --root "$node_root" adoption-plan \
    >"$work_root/symlink-marker.out" 2>"$work_root/symlink-marker.err"; then
  echo "adoption-plan unexpectedly accepted a symlink marker" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "unsafe-marker" and .error.category == "configuration"' \
  "$work_root/symlink-marker.err" >/dev/null
rm "$node_root/$marker"

if "$tool" --profile-dir "$profile_dir" --root "$node_root" --out-dir "$work_root/out" \
    adoption-plan >"$work_root/out-dir.out" 2>"$work_root/out-dir.err"; then
  echo "adoption-plan unexpectedly accepted --out-dir" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "unsupported-option" and .error.category == "usage"' \
  "$work_root/out-dir.err" >/dev/null

echo "Hermetic adoption plan tests passed."
