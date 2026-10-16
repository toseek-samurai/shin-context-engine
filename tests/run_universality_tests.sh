#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_universality_tests.sh <jq> <source-root> <report>" >&2
  exit 2
fi

jq_bin="$1"
source_root="$2"
report="$3"
contract="$source_root/docs/machine/universality-boundary-v1.json"
portable_contract="$source_root/docs/machine/portable-distribution-v1.json"
planner_contract="$source_root/docs/machine/adoption-planner-v1.json"
adoption_contract="$source_root/docs/machine/project-adoption-v1.json"
version_file="$source_root/VERSION"
candidate_policy="$source_root/docs/machine/shin-server-development-v1.json"

if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for universality validation: $jq_bin" >&2
  exit 2
fi
if [[ ! -f "$contract" || ! -f "$portable_contract" || ! -f "$planner_contract" || ! -f "$adoption_contract" || ! -f "$version_file" ]]; then
  echo "universality contract or VERSION is missing" >&2
  exit 2
fi

release_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
candidate_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-(dev|rc)\.(0|[1-9][0-9]*)$'

version_precedes() {
  local left="$1"
  local right="$2"
  local left_major left_minor left_patch right_major right_minor right_patch
  IFS=. read -r left_major left_minor left_patch <<<"$left"
  IFS=. read -r right_major right_minor right_patch <<<"$right"
  local index
  local -a left_parts=("$left_major" "$left_minor" "$left_patch")
  local -a right_parts=("$right_major" "$right_minor" "$right_patch")
  for index in 0 1 2; do
    if (( 10#${left_parts[index]} < 10#${right_parts[index]} )); then
      return 0
    fi
    if (( 10#${left_parts[index]} > 10#${right_parts[index]} )); then
      return 1
    fi
  done
  return 1
}

source_version="$(tr -d '\r\n' <"$version_file")"
contract_version="$source_version"
version_policy="stable"
if [[ "$source_version" =~ $candidate_pattern ]]; then
  if [[ ! -f "$candidate_policy" ]]; then
    echo "candidate VERSION requires an explicit server development policy" >&2
    exit 2
  fi
  candidate_identity="$("$jq_bin" -er '.developmentLane.coreVersion' "$candidate_policy")"
  if [[ "$candidate_identity" != "$source_version" ]]; then
    echo "candidate VERSION must match developmentLane.coreVersion" >&2
    exit 2
  fi
  contract_version="$("$jq_bin" -er '.developmentLane.contractBaselineVersion' "$candidate_policy")"
  if [[ ! "$contract_version" =~ $release_pattern ]]; then
    echo "candidate contract baseline must be a canonical release version" >&2
    exit 2
  fi
  candidate_release="${source_version%%-*}"
  if ! version_precedes "$contract_version" "$candidate_release"; then
    echo "candidate VERSION must be newer than its stable baseline" >&2
    exit 2
  fi
  version_policy="candidate-contract-baseline"
elif [[ ! "$source_version" =~ $release_pattern ]]; then
  echo "VERSION must be a canonical release or development candidate" >&2
  exit 2
fi

work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-universality.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT
mkdir -p "$(dirname "$report")"

bundle_list="$work_root/bundles.txt"
"$jq_bin" -r '.integrationBundles[]' "$contract" >"$bundle_list"
if [[ ! -s "$bundle_list" ]]; then
  echo "universality contract declares no integration bundles" >&2
  exit 1
fi

bundle_files=()
missing_bundle_files=()
while IFS= read -r relative; do
  if [[ -f "$source_root/$relative" ]]; then
    bundle_files+=("$source_root/$relative")
  else
    missing_bundle_files+=("$relative")
  fi
done <"$bundle_list"

if [[ ${#bundle_files[@]} -gt 0 ]]; then
  "$jq_bin" -s '.' "${bundle_files[@]}" >"$work_root/bundles.json"
else
  printf '[]\n' >"$work_root/bundles.json"
fi

find "$source_root/profiles" -maxdepth 1 -type f -name '*.json' -print \
  | sed 's#^.*/##; s#\.json$##' | LC_ALL=C sort >"$work_root/actual-profiles.txt"
"$jq_bin" -r '.core.profiles[].id' "$contract" | LC_ALL=C sort \
  >"$work_root/core-profiles.txt"
"$jq_bin" -r '.[].profiles[].id' "$work_root/bundles.json" | LC_ALL=C sort \
  >"$work_root/integration-profiles.txt"
cat "$work_root/core-profiles.txt" "$work_root/integration-profiles.txt" \
  | LC_ALL=C sort >"$work_root/classified-profiles.txt"

missing_profile_files=()
profile_id_mismatches=()
while IFS=$'\t' read -r id relative; do
  [[ -n "$id" && -n "$relative" ]] || continue
  if [[ ! -f "$source_root/$relative" ]]; then
    missing_profile_files+=("$relative")
    continue
  fi
  actual_id="$("$jq_bin" -r '.id // ""' "$source_root/$relative")"
  if [[ "$actual_id" != "$id" ]]; then
    profile_id_mismatches+=("$relative:$actual_id:$id")
  fi
done < <(
  {
    "$jq_bin" -r '.core.profiles[] | [.id, .path] | @tsv' "$contract"
    "$jq_bin" -r '.[] | .profiles[] | [.id, .path] | @tsv' "$work_root/bundles.json"
  }
)

source_violations="$work_root/source-violations.txt"
: >"$source_violations"
while IFS= read -r identifier; do
  [[ -n "$identifier" ]] || continue
  grep -R -n -i -F -- "$identifier" "$source_root/src" >>"$source_violations" || true
done < <("$jq_bin" -r '.core.forbiddenConsumerIdentifiers[]' "$contract")

release_plan="$work_root/core-release-plan.txt"
: >"$release_plan"
while IFS= read -r target; do
  make -C "$source_root" -n "$target" >>"$release_plan"
done < <("$jq_bin" -r '.core.releaseTargets[]' "$contract")

plan_violations="$work_root/plan-violations.txt"
: >"$plan_violations"
while IFS= read -r token; do
  [[ -n "$token" ]] || continue
  grep -n -i -F -- "$token" "$release_plan" >>"$plan_violations" || true
done < <("$jq_bin" -r '.core.forbiddenReleasePlanTokens[]' "$contract")

json_array_from_file() {
  "$jq_bin" -Rsc 'split("\n") | map(select(length > 0))' "$1"
}

printf '%s\n' "${missing_bundle_files[@]-}" \
  | "$jq_bin" -Rsc 'split("\n") | map(select(length > 0))' \
  >"$work_root/missing-bundles.json"
printf '%s\n' "${missing_profile_files[@]-}" \
  | "$jq_bin" -Rsc 'split("\n") | map(select(length > 0))' \
  >"$work_root/missing-profiles.json"
printf '%s\n' "${profile_id_mismatches[@]-}" \
  | "$jq_bin" -Rsc 'split("\n") | map(select(length > 0))' \
  >"$work_root/profile-mismatches.json"
json_array_from_file "$work_root/actual-profiles.txt" >"$work_root/actual-profiles.json"
json_array_from_file "$work_root/classified-profiles.txt" >"$work_root/classified-profiles.json"
json_array_from_file "$source_violations" >"$work_root/source-violations.json"
json_array_from_file "$plan_violations" >"$work_root/plan-violations.json"

"$jq_bin" -n \
  --arg source_version "$source_version" \
  --arg contract_version "$contract_version" \
  --arg version_policy "$version_policy" \
  --slurpfile contract "$contract" \
  --slurpfile portable "$portable_contract" \
  --slurpfile planner "$planner_contract" \
  --slurpfile adoption "$adoption_contract" \
  --slurpfile bundles "$work_root/bundles.json" \
  --slurpfile actual "$work_root/actual-profiles.json" \
  --slurpfile classified "$work_root/classified-profiles.json" \
  --slurpfile missing_bundles "$work_root/missing-bundles.json" \
  --slurpfile missing_profiles "$work_root/missing-profiles.json" \
  --slurpfile profile_mismatches "$work_root/profile-mismatches.json" \
  --slurpfile source_violations "$work_root/source-violations.json" \
  --slurpfile plan_violations "$work_root/plan-violations.json" '
    def check($id; $ok; $actual; $expected):
      {id: $id, ok: $ok, actual: $actual, expected: $expected};

    ($contract[0]) as $boundary |
    ($portable[0]) as $portable_distribution |
    ($planner[0]) as $adoption_planner |
    ($adoption[0]) as $project_adoption |
    ($bundles[0]) as $bundle_rows |
    ($actual[0]) as $actual_profiles |
    ($classified[0]) as $classified_profiles |
    ($classified_profiles | group_by(.) | map(select(length > 1) | .[0])) as $duplicates |
    ($bundle_rows | map(.id)) as $bundle_ids |
    ([
      check("contract-schema";
            $boundary.schemaVersion == "shin.universality-boundary.v1";
            $boundary.schemaVersion; "shin.universality-boundary.v1"),
      check("contract-status"; $boundary.status == "accepted";
            $boundary.status; "accepted"),
      check("contract-release"; $boundary.release == $contract_version;
            $boundary.release; $contract_version),
      check("portable-distribution-schema";
            $portable_distribution.schemaVersion == "shin.portable-distribution.v1";
            $portable_distribution.schemaVersion; "shin.portable-distribution.v1"),
      check("portable-distribution-release";
            $portable_distribution.release == $contract_version;
            $portable_distribution.release; $contract_version),
      check("adoption-planner-contract-schema";
            $adoption_planner.schemaVersion == "shin.adoption-planner-contract.v1";
            $adoption_planner.schemaVersion; "shin.adoption-planner-contract.v1"),
      check("adoption-planner-contract-release";
            $adoption_planner.release == $contract_version;
            $adoption_planner.release; $contract_version),
      check("adoption-planner-installed";
            any($portable_distribution.layout.machineContracts[];
                . == "contracts/adoption-planner-v1.json");
            $portable_distribution.layout.machineContracts; "contracts/adoption-planner-v1.json"),
      check("adoption-planner-boundary";
            $boundary.core.portableDistribution.adoptionPlannerContract == "docs/machine/adoption-planner-v1.json";
            $boundary.core.portableDistribution.adoptionPlannerContract;
            "docs/machine/adoption-planner-v1.json"),
      check("project-adoption-contract-schema";
            $project_adoption.schemaVersion == "shin.project-adoption-contract.v1";
            $project_adoption.schemaVersion; "shin.project-adoption-contract.v1"),
      check("project-adoption-contract-release";
            $project_adoption.release == $contract_version;
            $project_adoption.release; $contract_version),
      check("portable-profile-set";
            ($portable_distribution.layout.genericProfiles | map(split("/")[-1] | sub("\\.json$"; "")) | sort) ==
            ($boundary.core.profiles | map(select(.kind == "bootstrap") | .id) | sort);
            ($portable_distribution.layout.genericProfiles | map(split("/")[-1] | sub("\\.json$"; "")) | sort);
            ($boundary.core.profiles | map(select(.kind == "bootstrap") | .id) | sort)),
      check("portable-install-excludes-integrations";
            $portable_distribution.integrationPolicy.installedByDefault == false;
            $portable_distribution.integrationPolicy.installedByDefault; false),
      check("bundle-files-present"; ($missing_bundles[0] | length) == 0;
            $missing_bundles[0]; []),
      check("bundle-schemas";
            all($bundle_rows[]; .schemaVersion == "shin.integration-bundle.v1");
            ($bundle_rows | map(.schemaVersion)); ["shin.integration-bundle.v1"]),
      check("bundle-ids-unique"; ($bundle_ids | unique | length) == ($bundle_ids | length);
            $bundle_ids; ($bundle_ids | unique)),
      check("bundles-not-core-runtime-dependencies";
            all($bundle_rows[]; .coreRuntimeDependency == false);
            ($bundle_rows | map({id, coreRuntimeDependency})); false),
      check("bundles-not-core-release-dependencies";
            all($bundle_rows[]; .coreReleaseDependency == false);
            ($bundle_rows | map({id, coreReleaseDependency})); false),
      check("profile-files-present"; ($missing_profiles[0] | length) == 0;
            $missing_profiles[0]; []),
      check("profile-ids-match-files"; ($profile_mismatches[0] | length) == 0;
            $profile_mismatches[0]; []),
      check("profile-ownership-unique"; ($duplicates | length) == 0;
            $duplicates; []),
      check("profile-ownership-complete";
            ($actual_profiles | sort) == ($classified_profiles | unique | sort);
            ($classified_profiles | unique | sort); ($actual_profiles | sort)),
      check("consumer-identifiers-absent-from-core";
            ($source_violations[0] | length) == 0;
            $source_violations[0]; []),
      check("portable-release-plan-has-no-ecosystem-gates";
            ($plan_violations[0] | length) == 0;
            $plan_violations[0]; [])
    ]) as $checks |
    {
      schema_version: "shin.universality-check.v1",
      tool: "shin-universality-gate",
      command: "check",
      ok: all($checks[]; .ok),
      release: $source_version,
      contract_release: $contract_version,
      version_policy: $version_policy,
      counts: {
        core_profiles: ($boundary.core.profiles | length),
        integration_bundles: ($bundle_rows | length),
        integration_profiles: ($bundle_rows | map(.profiles | length) | add),
        registered_profiles: ($actual_profiles | length),
        source_violations: ($source_violations[0] | length),
        release_plan_violations: ($plan_violations[0] | length)
      },
      checks: $checks
    }
  ' >"$work_root/report.json"

mv "$work_root/report.json" "$report"
cat "$report"
"$jq_bin" -e '.ok == true' "$report" >/dev/null
