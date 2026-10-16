#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_adoption_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
version="$(<"$source_root/VERSION")"
IFS=. read -r major minor patch <<<"${version%%-*}"
above_current_release="$major.$minor.$((patch + 1))"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-adoption.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for adoption contract tests: $jq_bin" >&2
  exit 2
fi

"$jq_bin" -L "$source_root/tests" -en '
  include "consumer_adoption_version";
  [
    ["1.27.0", "1.27.0", true],
    ["1.27.2", "1.27.0", true],
    ["1.27.10", "1.27.2", true],
    ["1.27.2", "1.27.10", false],
    ["1.26.99", "1.27.0", false],
    ["2.0.0", "1.27.0", true],
    ["0.99.99", "1.27.0", false],
    ["1.27", "1.27.0", false],
    ["01.27.2", "1.27.0", false],
    ["1.27.3-dev.1", "1.27.0", false],
    [null, "1.27.0", false],
    ["1.27.2", null, false]
  ] | all(.[]; . as $case |
    (minimum_version_check("fixture"; $case[0]; $case[1]) | .ok) == $case[2])
' >/dev/null

project_root="$work_root/project"
profile_dir="$work_root/profiles"
marker="docs/machine/shin-project-adoption-v1.json"
mkdir -p "$project_root" "$profile_dir"
cp -R "$source_root/tests/fixtures/basic/root/." "$project_root/"
cp -R "$source_root/tests/fixtures/adoption/root/." "$project_root/"
cp "$source_root/tests/fixtures/basic/profiles/fixture.json" "$profile_dir/fixture.json"
cp "$source_root/profiles/generic-source.json" "$profile_dir/generic-source.json"
"$jq_bin" --arg marker "$marker" '
  .agentStartPaths += ["AGENTS.md", $marker] |
  .agentStartPaths |= unique
' "$profile_dir/fixture.json" >"$work_root/fixture-profile.json"
mv "$work_root/fixture-profile.json" "$profile_dir/fixture.json"

run_check() {
  local output="$1"
  "$tool" --profile-dir "$profile_dir" --root "$project_root" \
    adoption-check --marker "$marker" --strict >"$output"
}

run_invalid_document() {
  local id="$1"
  local filter="$2"
  local expected_code="$3"
  local original="$project_root/$marker"
  cp "$original" "$work_root/original-marker.json"
  "$jq_bin" "$filter" "$work_root/original-marker.json" >"$original"
  if run_check "$work_root/$id.json"; then
    echo "invalid adoption case unexpectedly passed: $id" >&2
    exit 1
  fi
  "$jq_bin" -e --arg code "$expected_code" \
    '.ok == false and any(.issues[]; .code == $code)' "$work_root/$id.json" >/dev/null
  cp "$work_root/original-marker.json" "$original"
}

run_check "$work_root/valid.json"
"$jq_bin" -e '
  .schema_version == "shin.project-adoption-check.v1" and
  .ok == true and
  .strict == true and
  .project.id == "fixture-project" and
  .profile.id == "fixture" and
  .profile.bootstrap_only == false and
  .profile.eval_cases > 0 and
  .profile.pack_eval_cases > 0 and
  .profile.marker_registered == true and
  .profile.agent_entry_registered == true and
  .quality_gates == {
    profile_check_strict: true,
    check: true,
    doctor_strict: true,
    eval_strict: true,
    pack_eval_strict: true
  } and
  .agent == {
    entry_path: "AGENTS.md",
    context_epoch_reference_required: true,
    marker_reference_required: true,
    profile_reference_required: true
  } and
  .counts == {errors: 0, warnings: 0, extensions: 1}
' "$work_root/valid.json" >/dev/null

cp "$project_root/$marker" "$work_root/original-marker.json"
"$jq_bin" '.engine.minimumVersion = "1.25.0"' "$work_root/original-marker.json" >"$project_root/$marker"
run_check "$work_root/prerelease-satisfies-lower-minimum.json"
"$jq_bin" -e --arg version "$version" '
  .ok and
  .engine.version == $version and
  .engine.minimum_version == "1.25.0"
' "$work_root/prerelease-satisfies-lower-minimum.json" >/dev/null
cp "$work_root/original-marker.json" "$project_root/$marker"

"$tool" --profile-dir "$profile_dir" --root "$project_root" \
  adoption-check --marker "$project_root/$marker" --strict \
  >"$work_root/absolute-marker-inside-root.json"
"$jq_bin" -e --arg marker "$marker" \
  '.ok and .marker == $marker and .profile.marker_registered' \
  "$work_root/absolute-marker-inside-root.json" >/dev/null
"$tool" --profile-dir "$profile_dir" --root "$project_root" \
  adoption-check --marker "./docs/machine/../machine/shin-project-adoption-v1.json" --strict \
  >"$work_root/dot-marker-inside-root.json"
"$jq_bin" -e --arg marker "$marker" '.ok and .marker == $marker' \
  "$work_root/dot-marker-inside-root.json" >/dev/null

run_invalid_document unknown-field '.unexpected = true' unknown-adoption-field
run_invalid_document future-version '.engine.minimumVersion = "99.0.0"' engine-version-too-old
run_invalid_document prerelease-minimum '.engine.minimumVersion = "2.0.0-dev.1"' invalid-minimum-version
run_invalid_document next-release-ceiling ".engine.minimumVersion = \"$above_current_release\"" engine-version-too-old
if [[ "$version" == *-* ]]; then
    run_invalid_document prerelease-ceiling ".engine.minimumVersion = \"${version%%-*}\"" engine-version-too-old
fi
run_invalid_document generator-mismatch '.engine.generatorIdentity = "other-generator"' generator-identity-mismatch
run_invalid_document absolute-agent-path '.agent.entryPath = "/tmp/AGENTS.md"' invalid-agent-entry-path
run_invalid_document scalar-extension '.extensions["fixture.test"] = "opaque-but-not-object"' invalid-extension-value
run_invalid_document unnamespaced-extension '.extensions = {legacy: {enabled: true}}' invalid-extension-identifier
run_invalid_document malformed-extension '.extensions = {"fixture..legacy": {enabled: true}}' invalid-extension-identifier

cp "$profile_dir/fixture.json" "$work_root/fixture-profile.json"
"$jq_bin" --arg marker "$marker" '.agentStartPaths -= [$marker]' \
  "$work_root/fixture-profile.json" >"$profile_dir/fixture.json"
if run_check "$work_root/profile-marker-routing.json"; then
  echo "profile without adoption marker routing unexpectedly passed" >&2
  exit 1
fi
"$jq_bin" -e 'any(.issues[]; .code == "profile-adoption-marker-missing")' \
  "$work_root/profile-marker-routing.json" >/dev/null
"$jq_bin" '.agentStartPaths += ["docs/machine/shin-project-adoption-v1.json"] | .agentStartPaths -= ["AGENTS.md"]' \
  "$work_root/fixture-profile.json" >"$profile_dir/fixture.json"
if run_check "$work_root/profile-agent-routing.json"; then
  echo "profile without agent entry routing unexpectedly passed" >&2
  exit 1
fi
"$jq_bin" -e 'any(.issues[]; .code == "profile-agent-entry-missing")' \
  "$work_root/profile-agent-routing.json" >/dev/null
cp "$work_root/fixture-profile.json" "$profile_dir/fixture.json"

cp "$project_root/$marker" "$work_root/permanent-marker.json"
"$jq_bin" '.profile.id = "generic-source" | .index.key = "generic-source"' \
  "$work_root/permanent-marker.json" >"$project_root/$marker"
printf '%s\n' \
  '# Fixture Agent Contract' \
  'Context epoch: `fixture-shin-v1.12-2026-07-19`.' \
  'Read `docs/machine/shin-project-adoption-v1.json` and use permanent profile `generic-source`.' \
  >"$project_root/AGENTS.md"
if run_check "$work_root/bootstrap-profile.json"; then
  echo "bootstrap profile unexpectedly passed permanent adoption" >&2
  exit 1
fi
"$jq_bin" -e 'any(.issues[]; .code == "bootstrap-profile-adopted-as-permanent")' \
  "$work_root/bootstrap-profile.json" >/dev/null
cp "$work_root/permanent-marker.json" "$project_root/$marker"
cp "$source_root/tests/fixtures/adoption/root/AGENTS.md" "$project_root/AGENTS.md"

printf '%s\n' '# Fixture Agent Contract without routing references' >"$project_root/AGENTS.md"
if run_check "$work_root/agent-references.json"; then
  echo "missing agent references unexpectedly passed adoption" >&2
  exit 1
fi
"$jq_bin" -e '
  any(.issues[]; .code == "agent-epoch-reference-missing") and
  any(.issues[]; .code == "agent-marker-reference-missing") and
  any(.issues[]; .code == "agent-profile-reference-missing")
' "$work_root/agent-references.json" >/dev/null
cp "$source_root/tests/fixtures/adoption/root/AGENTS.md" "$project_root/AGENTS.md"

mv "$project_root/$marker" "$work_root/real-marker.json"
ln -s "$work_root/real-marker.json" "$project_root/$marker"
if "$tool" --profile-dir "$profile_dir" --root "$project_root" \
    adoption-check --marker "$marker" --strict >"$work_root/symlink.out" 2>"$work_root/symlink.err"; then
  echo "symlinked adoption marker unexpectedly passed" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "unsafe-adoption-marker"' "$work_root/symlink.err" >/dev/null
rm "$project_root/$marker"
mv "$work_root/real-marker.json" "$project_root/$marker"

if "$tool" --profile fixture --profile-dir "$profile_dir" --root "$project_root" \
    adoption-check --marker "$marker" --strict >"$work_root/profile-option.out" \
    2>"$work_root/profile-option.err"; then
  echo "adoption-check unexpectedly accepted --profile" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "unsupported-option" and .error.category == "usage"' \
  "$work_root/profile-option.err" >/dev/null

if "$tool" --profile-dir "$profile_dir" --root "$project_root" \
    adoption-check --marker /tmp/adoption.json --strict >"$work_root/absolute-marker.out" \
    2>"$work_root/absolute-marker.err"; then
  echo "adoption-check unexpectedly accepted a marker outside --root" >&2
  exit 1
fi
"$jq_bin" -e '.error.code == "repository-file-outside-root" and
  .error.category == "usage" and
  (.error.remediation | contains("--marker <relative-file>"))' \
  "$work_root/absolute-marker.err" >/dev/null

echo "Hermetic project adoption tests passed."
