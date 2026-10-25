#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_cold_start_tests.sh <shin-context> <source-root>" >&2
  exit 2
fi

tool="$1"
source_root="$2"
fixture="$source_root/tests/fixtures/cold-start"
profile_dir="$source_root/profiles"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-context-cold-start.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

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

prepare_root() {
  local name="$1"
  local root="$work_root/$name"
  cp -R "$fixture/$name" "$root"
  printf '%s\n' "$root"
}

validate_source_profile() {
  local name="$1"
  local profile="$2"
  local query="$3"
  local expected_path="$4"
  local expected_type="$5"
  local ignored_symbol="$6"
  local root
  root="$(prepare_root "$name")"
  local out="$work_root/out-$name"
  if [[ "$name" == "node" ]]; then
    ln -s "$fixture/node/src/account.ts" "$root/src/external-link.ts"
  fi

  "$tool" --root "$root" discover >"$work_root/discover-$name.json"
  assert_contains "$work_root/discover-$name.json" '"state":"source-only"'
  assert_contains "$work_root/discover-$name.json" "\"primary_profile\":\"$profile\""
  assert_contains "$work_root/discover-$name.json" '"project_mutation_required":false'

  "$tool" --profile "$profile" --profile-dir "$profile_dir" --root "$root" profile-check --strict \
    >"$work_root/profile-$name.json"
  assert_contains "$work_root/profile-$name.json" '"ok": true'
  assert_contains "$work_root/profile-$name.json" 'bootstrap-profile-without-eval'

  "$tool" --profile "$profile" --profile-dir "$profile_dir" --root "$root" --out-dir "$out" build \
    >"$work_root/build-$name.json"
  "$tool" --profile "$profile" --profile-dir "$profile_dir" --root "$root" --out-dir "$out" check \
    >"$work_root/check-$name.json"
  "$tool" --profile "$profile" --profile-dir "$profile_dir" --root "$root" --out-dir "$out" doctor --strict \
    >"$work_root/doctor-$name.json"
  "$tool" --profile "$profile" --profile-dir "$profile_dir" --root "$root" --out-dir "$out" \
    brief "$query" --require-current >"$work_root/brief-$name.json"

  assert_contains "$work_root/build-$name.json" '"ok": true'
  assert_contains "$work_root/check-$name.json" '"current": true'
  assert_contains "$work_root/doctor-$name.json" '"ok": true'
  assert_contains "$work_root/brief-$name.json" "$expected_path"
  assert_contains "$work_root/brief-$name.json" "$expected_type"
  assert_contains "$out/docs/kb/generated/manifest.json" '"bootstrap_only": true'
  if [[ -n "$ignored_symbol" ]]; then
    assert_not_contains "$out/docs/kb/generated/facts.jsonl" "$ignored_symbol"
  fi
  assert_not_contains "$out/docs/kb/generated/facts.jsonl" 'src/external-link.ts'
}

validate_source_profile java generic-java "AccountService class" \
  "src/main/java/example/AccountService.java" "java-class" "GeneratedNoise"
validate_source_profile kotlin generic-kotlin "loadAccount Kotlin function" \
  "src/main/kotlin/example/AccountService.kt" "kotlin-function" "GeneratedNoise"
assert_contains "$work_root/out-kotlin/docs/kb/generated/facts.jsonl" 'kotlin value-class AccountKey'
assert_contains "$work_root/out-kotlin/docs/kb/generated/facts.jsonl" 'kotlin test-case loads an account'
assert_not_contains "$work_root/out-kotlin/docs/kb/generated/facts.jsonl" 'kotlin function fun'
validate_source_profile node generic-node "loadAccount arrow function" \
  "src/account.ts" "ts-arrow-function" "DependencyNoise"
validate_source_profile cpp generic-cpp "load_account cpp function" \
  "src/account.cxx" "cpp-function" "dependency_noise"
validate_source_profile dotnet generic-dotnet "LoadAccount csharp method" \
  "src/AccountService.cs" "csharp-method" "GeneratedNoise"
validate_source_profile retool generic-retool "ColdStartQuery JavascriptQuery query" \
  "apps/Fixture/main.rsx" "retool-query" ""

case_alias_root="$(prepare_root kotlin)"
case_alias_out="$work_root/out-kotlin-case-alias"
printf '%s\n' '# Kotlin case alias fixture' >"$case_alias_root/README.md"
"$tool" --profile generic-kotlin --profile-dir "$profile_dir" --root "$case_alias_root" \
  --out-dir "$case_alias_out" build >"$work_root/build-kotlin-case-alias.json"
"$tool" --profile generic-kotlin --profile-dir "$profile_dir" --root "$case_alias_root" \
  --out-dir "$case_alias_out" doctor --strict >"$work_root/doctor-kotlin-case-alias.json"
assert_contains "$work_root/doctor-kotlin-case-alias.json" '"ok": true'
assert_contains "$case_alias_out/docs/kb/generated/documents.jsonl" '"path":"README.md"'
assert_not_contains "$case_alias_out/docs/kb/generated/documents.jsonl" '"path":"readme.md"'

kotlin_dominant_root="$(prepare_root kotlin)"
mkdir -p "$kotlin_dominant_root/frontend"
printf '%s\n' 'export const helper = 1;' >"$kotlin_dominant_root/frontend/helper.ts"
for index in 1 2 3 4 5 6 7 8; do
  cp "$fixture/kotlin/src/main/kotlin/example/AccountService.kt" \
    "$kotlin_dominant_root/src/main/kotlin/example/Dominant$index.kt"
done
"$tool" --root "$kotlin_dominant_root" discover >"$work_root/discover-kotlin-dominant.json"
assert_contains "$work_root/discover-kotlin-dominant.json" '"primary_profile":"generic-kotlin"'
assert_contains "$work_root/discover-kotlin-dominant.json" '"recommended_profiles": ["generic-kotlin","generic-source","generic-node"]'

gradle_metadata_root="$work_root/gradle-metadata-only"
mkdir -p "$gradle_metadata_root"
cp "$fixture/kotlin/build.gradle.kts" "$fixture/kotlin/settings.gradle.kts" "$gradle_metadata_root/"
"$tool" --root "$gradle_metadata_root" discover >"$work_root/discover-gradle-metadata-only.json"
assert_contains "$work_root/discover-gradle-metadata-only.json" '"build_systems": ["gradle"]'
assert_contains "$work_root/discover-gradle-metadata-only.json" '"kotlin_files":0'
assert_contains "$work_root/discover-gradle-metadata-only.json" '"state":"empty"'
assert_contains "$work_root/discover-gradle-metadata-only.json" '"primary_profile":null'

mixed_root="$(prepare_root mixed)"
mixed_out="$work_root/out-mixed"
"$tool" --root "$mixed_root" discover >"$work_root/discover-mixed.json"
assert_contains "$work_root/discover-mixed.json" '"primary_profile":"generic-source"'
"$tool" --profile generic-source --profile-dir "$profile_dir" --root "$mixed_root" profile-check --strict \
  >"$work_root/profile-mixed.json"
"$tool" --profile generic-source --profile-dir "$profile_dir" --root "$mixed_root" --out-dir "$mixed_out" build \
  >"$work_root/build-mixed.json"
assert_contains "$mixed_out/docs/kb/generated/facts.jsonl" 'java record Account'
assert_contains "$mixed_out/docs/kb/generated/facts.jsonl" 'ts function formatAccount'

docs_root="$(prepare_root docs)"
docs_out="$work_root/out-docs"
"$tool" --root "$docs_root" discover >"$work_root/discover-docs.json"
assert_contains "$work_root/discover-docs.json" '"state":"documented"'
assert_contains "$work_root/discover-docs.json" '"primary_profile":"generic-docs"'
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$docs_root" profile-check --strict \
  >"$work_root/profile-docs.json"
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$docs_root" --out-dir "$docs_out" build \
  >"$work_root/build-docs.json"
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$docs_root" --out-dir "$docs_out" \
  brief "cold start architecture" --require-current >"$work_root/brief-docs.json"
assert_contains "$work_root/brief-docs.json" 'docs/architecture.md'

broken_docs_root="$(prepare_root docs)"
broken_docs_out="$work_root/out-broken-docs"
printf '\n[Missing bootstrap target](docs/missing-bootstrap-target.md)\n' >>"$broken_docs_root/README.md"
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$broken_docs_root" \
  --out-dir "$broken_docs_out" build >"$work_root/build-broken-docs.json"
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$broken_docs_root" \
  --out-dir "$broken_docs_out" check >"$work_root/check-broken-docs.json"
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$broken_docs_root" \
  --out-dir "$broken_docs_out" doctor >"$work_root/doctor-broken-docs.json"
expect_failure "$work_root/doctor-strict-broken-docs.json" \
  "$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$broken_docs_root" \
    --out-dir "$broken_docs_out" doctor --strict
"$tool" --profile generic-docs --profile-dir "$profile_dir" --root "$broken_docs_root" \
  --out-dir "$broken_docs_out" brief "cold start architecture" --require-current \
  >"$work_root/brief-broken-docs.json"
assert_contains "$work_root/build-broken-docs.json" '"published": true'
assert_contains "$work_root/build-broken-docs.json" '"local_link_policy": "diagnostic"'
assert_contains "$work_root/build-broken-docs.json" '"severity": "warning"'
assert_contains "$work_root/check-broken-docs.json" '"current": true'
assert_contains "$work_root/check-broken-docs.json" '"broken_local_links": 1'
assert_contains "$work_root/doctor-broken-docs.json" '"ok": true'
assert_contains "$work_root/doctor-broken-docs.json" '"severity":"warning"'
assert_contains "$work_root/doctor-strict-broken-docs.json" '"ok": false'
assert_contains "$work_root/brief-broken-docs.json" '"current":true'
assert_contains "$work_root/brief-broken-docs.json" 'broken local links recorded as diagnostics'

empty_root="$(prepare_root empty)"
ln -s "$fixture/node/package.json" "$empty_root/package.json"
"$tool" --root "$empty_root" discover >"$work_root/discover-empty.json"
assert_contains "$work_root/discover-empty.json" '"state":"empty"'
assert_contains "$work_root/discover-empty.json" '"indexable":false'
assert_contains "$work_root/discover-empty.json" '"primary_profile":null'
assert_contains "$work_root/discover-empty.json" '"recommended_profiles": []'
assert_contains "$work_root/discover-empty.json" 'no-indexable-content'

metadata_root="$work_root/metadata-only"
mkdir -p "$metadata_root"
printf '%s\n' '{"name":"metadata-only","private":true}' >"$metadata_root/package.json"
"$tool" --root "$metadata_root" discover >"$work_root/discover-metadata-only.json"
assert_contains "$work_root/discover-metadata-only.json" '"build_systems": ["node"]'
assert_contains "$work_root/discover-metadata-only.json" '"state":"empty"'
assert_contains "$work_root/discover-metadata-only.json" '"indexable":false'

partial_retool_root="$work_root/partial-retool"
mkdir -p "$partial_retool_root/apps/Partial"
cp "$fixture/retool/apps/Fixture/main.rsx" "$partial_retool_root/apps/Partial/main.rsx"
"$tool" --root "$partial_retool_root" discover >"$work_root/discover-partial-retool.json"
assert_contains "$work_root/discover-partial-retool.json" '"primary_profile":"generic-retool"'
"$tool" --profile generic-retool --profile-dir "$profile_dir" --root "$partial_retool_root" \
  profile-check --strict >"$work_root/profile-partial-retool.json"
assert_contains "$work_root/profile-partial-retool.json" '"ok": true'
assert_contains "$work_root/profile-partial-retool.json" 'bootstrap-source-fact-root-missing'

for profile in generic-java generic-kotlin generic-node generic-cpp generic-dotnet generic-retool generic-source generic-docs; do
  if [[ ! -f "$profile_dir/$profile.json" ]]; then
    echo "discoverable profile is missing: $profile" >&2
    exit 1
  fi
done

echo "Hermetic cold-start tests passed."
