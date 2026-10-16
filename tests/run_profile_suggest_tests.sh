#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_profile_suggest_tests.sh <jq> <beta-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-profile-suggest.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

alpha_root="$work_root/alpha-project"
beta_root="$work_root/beta-project"
profile_dir="$work_root/profiles"
config="$work_root/routing.json"
mkdir -p "$alpha_root" "$beta_root" "$profile_dir"
mkdir -p "$alpha_root/docs/machine" "$beta_root/docs/machine"
printf '# Alpha project\n\nSynthetic route fixture.\n' >"$alpha_root/README.md"
printf '# Beta project\n\nSynthetic route fixture.\n' >"$beta_root/README.md"
for route in alpha beta; do
  "$jq_bin" -n --arg id "route-$route" --arg label "Fixture $route route" '{
    schemaVersion: "shin.profile.v1",
    document: "synthetic-profile-routing-fixture",
    status: "active",
    summary: "Minimal synthetic profile for route-selection contract tests.",
    id: $id,
    sourceLabel: $label,
    rootFiles: ["README.md"],
    optionalRootFiles: [],
    sourceExtensions: [".md", ".json"],
    documentExcludePrefixes: [],
    secretScanExcludePrefixes: [],
    sourceFactRoots: [],
    sourceFactExtensions: [],
    sourceFactExtractors: [],
    agentStartPaths: ["README.md"],
    markdownMaxHeadingLevel: 4,
    markdownChunkLines: 3,
    markdownChunkMinLines: 4,
    markdownChunkChars: 240,
    jsonKeySections: true,
    jsonKeyIncludePrefixes: [],
    jsonKeyMaxDepth: 2,
    jsonKeyMaxSectionsPerFile: 4,
    jsonlMaxRowsPerFile: 3,
    jsonPathReferences: true,
    jsonSchemaTitle: true,
    jsonSummaryKeys: true,
    sortTopics: true,
    defaultTopic: "general",
    aliases: [],
    evalCases: [],
    packEvalCases: [],
    topicRules: []
  }' >"$profile_dir/route-$route.json"
done

mkdir -p "$alpha_root/routing" "$alpha_root/docs/machine"
"$jq_bin" -n '{schemaVersion:"fixture.contract.v1",id:"routing-target"}' \
  >"$alpha_root/docs/machine/routing-target.json"
"$jq_bin" -n \
  '{schemaVersion:"fixture.contract.v1",reference:"docs/machine/routing-target.json"}' \
  >"$alpha_root/routing/ordinary-contract.json"
"$jq_bin" -n '
  {
    schemaVersion: "shin.profile-routing.v1",
    id: "indexed-routing-fixture",
    routes: [{
      id: "external-project",
      description: "Marker is relative to the configured external route root.",
      profile: "route-alpha",
      profileDir: "../profiles",
      root: "../external-project",
      marker: "docs/machine/external-adoption.json",
      signals: ["external project"]
    }]
  }
' >"$alpha_root/routing/indexed-routing.json"
cp "$alpha_root/routing/indexed-routing.json" "$alpha_root/external-routing.json"
"$jq_bin" '.rootFiles += [
    "docs/machine/routing-target.json",
    "routing/ordinary-contract.json",
    "routing/indexed-routing.json",
    "external-routing.json"
  ]' \
  "$profile_dir/route-alpha.json" >"$work_root/route-alpha-with-routing.json"
mv "$work_root/route-alpha-with-routing.json" "$profile_dir/route-alpha.json"

"$jq_bin" -n \
  --arg profile_dir "$profile_dir" \
  --arg alpha_root "$alpha_root" \
  --arg beta_root "$beta_root" '
  {
    schemaVersion: "shin.profile-routing.v1",
    id: "fixture-routing",
    routes: [
      {
        id: "alpha",
        description: "Fixture project route",
        profile: "route-alpha",
        profileDir: $profile_dir,
        root: $alpha_root,
        signals: ["alpha", "alpha admin", "data console", "api service", "shared runtime"]
      },
      {
        id: "beta",
        description: "Fixture Example Engine route",
        profile: "route-beta",
        profileDir: $profile_dir,
        root: $beta_root,
        signals: ["beta", "example engine", "project status", "agent context pack", "shared runtime"]
      }
    ]
  }' >"$config"

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

"$tool" suggest-profile --config "$config" --from-root "$alpha_root" \
  'Beta Example Engine project status' >"$work_root/beta-suggestion.json"
"$tool" suggest-profile --config "$config" --from-root "$alpha_root" \
  'Beta Example Engine project status' >"$work_root/beta-suggestion-repeat.json"
diff "$work_root/beta-suggestion.json" "$work_root/beta-suggestion-repeat.json"
assert_json "$work_root/beta-suggestion.json" '
  .schema_version == "shin.profile-suggestion.v1" and .ok and
  .state == "recommended" and .policy.matching == "explicit-signal-all-terms-v1" and
  (.policy.automatic_switch | not) and (.policy.index_access | not) and
  (.policy.project_mutation | not) and
  .current_route.route_id == "alpha" and
  .from_root == .current_route.root and
  .recommendation.route_id == "beta" and .recommendation.profile == "route-beta" and
  .recommendation.current_route == false and .switch_recommended and
  .recommendation.matched_signals == ["beta", "example engine", "project status"] and
  .matched_routes == 1 and .returned_routes == 1 and
  (.config.content_hash | startswith("fnv1a64:"))'

"$tool" suggest-profile --config "$config" --from-root "$alpha_root" \
  'Alpha data console API service' >"$work_root/current-suggestion.json"
assert_json "$work_root/current-suggestion.json" '
  .state == "current-route" and .current_route.route_id == "alpha" and
  .from_root == .current_route.root and
  .recommendation.route_id == "alpha" and .recommendation.current_route and
  (.switch_recommended | not)'

ln -s "$alpha_root" "$work_root/alpha-root-link"
"$tool" suggest-profile --config "$config" --from-root "$work_root/alpha-root-link" \
  'Alpha data console API service' >"$work_root/symlink-root-suggestion.json"
assert_json "$work_root/symlink-root-suggestion.json" '
  .state == "current-route" and .from_root == .current_route.root and
  .recommendation.route_id == "alpha"'

"$tool" suggest-profile --config "$config" --from-root "$alpha_root" \
  'shared runtime' >"$work_root/ambiguous.json"
assert_json "$work_root/ambiguous.json" '
  .state == "ambiguous" and .recommendation == null and
  (.switch_recommended | not) and .matched_routes == 2 and .returned_routes == 2 and
  [.candidates[].route_id] == ["alpha", "beta"]'

"$tool" suggest-profile --config "$config" --from-root "$alpha_root" \
  --limit 1 'shared runtime' >"$work_root/ambiguous-limited.json"
assert_json "$work_root/ambiguous-limited.json" '
  .state == "ambiguous" and .recommendation == null and
  .matched_routes == 2 and .returned_routes == 1 and .limit == 1'

"$tool" suggest-profile --config "$config" 'unrelated unknown question' \
  >"$work_root/no-match.json"
assert_json "$work_root/no-match.json" '
  .state == "no-match" and .current_route == null and .recommendation == null and
  .matched_routes == 0 and .returned_routes == 0 and (.switch_recommended | not)'

if find "$alpha_root" "$beta_root" -path '*/docs/kb/generated' -print -quit | grep -q .; then
  echo "suggest-profile unexpectedly created a generated index" >&2
  exit 1
fi

"$tool" --profile route-alpha --profile-dir "$profile_dir" --root "$alpha_root" \
  --out-dir "$work_root/indexed-routing-context" build \
  >"$work_root/indexed-routing-build.json"
assert_json "$work_root/indexed-routing-build.json" '
  .schema_version == "shin.build.v1" and .ok and .published and
  .counts.documents > 0
'
"$jq_bin" -s -e '
  any(.[]; .path == "routing/ordinary-contract.json" and
           .target == "docs/machine/routing-target.json" and .status == "ok") and
  (any(.[]; .path == "routing/indexed-routing.json" or
            .path == "external-routing.json") | not)
' "$work_root/indexed-routing-context/docs/kb/generated/links.jsonl" >/dev/null

"$jq_bin" '.unexpected = true' "$config" >"$work_root/unknown-field.json"
expect_failure "$work_root/unknown-field-result.json" \
  "$tool" suggest-profile --config "$work_root/unknown-field.json" fixture
assert_json "$work_root/unknown-field-result.json" '
  .error.code == "invalid-profile-routing-config" and
  .error.category == "configuration"'

"$jq_bin" '.routes[1].root = .routes[0].root' "$config" \
  >"$work_root/duplicate-root.json"
expect_failure "$work_root/duplicate-root-result.json" \
  "$tool" suggest-profile --config "$work_root/duplicate-root.json" fixture
assert_json "$work_root/duplicate-root-result.json" '
  .error.code == "duplicate-profile-route-root" and
  .error.category == "configuration"'

ln -s "$config" "$work_root/routing-symlink.json"
expect_failure "$work_root/symlink-result.json" \
  "$tool" suggest-profile --config "$work_root/routing-symlink.json" fixture
assert_json "$work_root/symlink-result.json" '
  .error.code == "invalid-profile-routing-config" and
  .error.category == "configuration"'

expect_failure "$work_root/global-root-result.json" \
  "$tool" --root "$alpha_root" suggest-profile --config "$config" fixture
assert_json "$work_root/global-root-result.json" '
  .error.code == "unsupported-option" and .error.category == "usage"'
expect_failure "$work_root/missing-config-result.json" "$tool" suggest-profile fixture
assert_json "$work_root/missing-config-result.json" '
  .error.code == "missing-option" and .error.category == "usage"'
expect_failure "$work_root/missing-query-result.json" \
  "$tool" suggest-profile --config "$config"
assert_json "$work_root/missing-query-result.json" '
  .error.code == "missing-query" and .error.category == "usage"'

echo "Hermetic profile suggestion tests passed."
