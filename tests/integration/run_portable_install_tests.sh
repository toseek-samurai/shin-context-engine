#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: run_portable_install_tests.sh <cmake> <jq> <source-root> <cmake-build-dir> <report>" >&2
  exit 2
fi

cmake_bin="$1"
jq_bin="$2"
source_root="$(cd "$3" && pwd -P)"
cmake_build_dir="$4"
report="$5"

if ! command -v "$cmake_bin" >/dev/null 2>&1; then
  echo "cmake is required for portable install validation: $cmake_bin" >&2
  exit 2
fi
if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for portable install validation: $jq_bin" >&2
  exit 2
fi
if [[ ! -d "$cmake_build_dir" ]]; then
  echo "configured CMake build directory is missing: $cmake_build_dir" >&2
  exit 2
fi

work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-portable-install.XXXXXX")"
cleanup() {
  local status=$?
  trap - EXIT
  if [[ $status -ne 0 ]]; then
    echo "portable install gate failed; available command results:" >&2
    for result in adoption-plan adoption profile-suggestion profile-check build check worktree-diff doctor eval pack-eval validate validate-summary ready brief inspect read file-read pack receipt-scope receipt-import receipt-check index-list-before index-prune-dry index-prune-apply index-list; do
      if [[ -f "$work_root/$result.json" ]]; then
        echo "--- $result ---" >&2
        cat "$work_root/$result.json" >&2
      fi
    done
  fi
  rm -rf "$work_root"
  exit "$status"
}
trap cleanup EXIT
install_prefix="$work_root/install-prefix"
relocated_prefix="$work_root/relocated-prefix"

"$cmake_bin" --install "$cmake_build_dir" --prefix "$install_prefix" >"$work_root/install.log"
mv "$install_prefix" "$relocated_prefix"

tool="$relocated_prefix/bin/shin-context"
share_root="$relocated_prefix/share/shin-context"
profile_dir="$share_root/profiles"
contract_dir="$share_root/contracts"
distribution="$contract_dir/portable-distribution-v1.json"

for required in \
  "$tool" \
  "$share_root/VERSION" \
  "$distribution" \
  "$contract_dir/adoption-planner-v1.json" \
  "$contract_dir/agent-readiness-v1.json" \
  "$contract_dir/aggregate-validation-v1.json" \
  "$contract_dir/evidence-provenance-v1.json" \
  "$contract_dir/hermes-agent-integration-v1.json" \
  "$contract_dir/identifier-evidence-v1.json" \
  "$contract_dir/project-adoption-v1.json" \
  "$contract_dir/profile-routing-v1.json" \
  "$contract_dir/index-lifecycle-v1.json" \
  "$contract_dir/trust-boundary-hardening-v1.json" \
  "$contract_dir/worktree-diff-v1.json" \
  "$contract_dir/source-read-v1.json" \
  "$contract_dir/source-file-read-v1.json" \
  "$share_root/licenses/nlohmann/LICENSE.MIT" \
  "$share_root/licenses/simdjson/LICENSE-APACHE-2.0" \
  "$share_root/licenses/simdjson/LICENSE-MIT" \
  "$share_root/licenses/tree-sitter-cpp/LICENSE" \
  "$share_root/licenses/tree-sitter-cpp/UPSTREAM.json" \
  "$share_root/codex/skills/shin-context/SKILL.md" \
  "$share_root/codex/skills/shin-context/references/trust-boundaries.md" \
  "$share_root/codex/skills/shin-context/agents/openai.yaml" \
  "$share_root/hermes/plugins/shin-context/plugin.yaml" \
  "$share_root/hermes/plugins/shin-context/__init__.py" \
  "$share_root/hermes/plugins/shin-context/adapter.py" \
  "$share_root/hermes/plugins/shin-context/model.py" \
  "$share_root/hermes/plugins/shin-context/resolution.py" \
  "$share_root/hermes/plugins/shin-context/runtime.py" \
  "$share_root/hermes/plugins/shin-context/schemas.py" \
  "$share_root/hermes/plugins/shin-context/validation.py" \
  "$share_root/hermes/plugins/shin-context/skills/workflow/SKILL.md"; do
  if [[ ! -f "$required" ]]; then
    echo "portable install is missing required file: $required" >&2
    exit 1
  fi
done
if [[ ! -x "$tool" ]]; then
  echo "installed Shin binary is not executable: $tool" >&2
  exit 1
fi
if ! grep -Fq 'Use Shin v2.0.0 or newer' "$share_root/codex/skills/shin-context/SKILL.md" ||
   ! grep -Fq 'Require Shin v2.0.0 or newer' "$share_root/hermes/plugins/shin-context/skills/workflow/SKILL.md"; then
  echo "portable integration skills must match the current v2.0 source guidance" >&2
  exit 1
fi

"$jq_bin" -r '.layout.genericProfiles[] | split("/")[-1] | sub("\\.json$"; "")' \
  "$distribution" | LC_ALL=C sort >"$work_root/expected-profiles.txt"
find "$profile_dir" -maxdepth 1 -type f -name '*.json' -print \
  | sed 's#^.*/##; s#\.json$##' | LC_ALL=C sort >"$work_root/actual-profiles.txt"
if ! diff -u "$work_root/expected-profiles.txt" "$work_root/actual-profiles.txt"; then
  echo "portable profile registry differs from its distribution contract" >&2
  exit 1
fi

while IFS= read -r relative; do
  if [[ ! -f "$share_root/$relative" ]]; then
    echo "portable contract declares a missing installed artifact: $relative" >&2
    exit 1
  fi
done < <("$jq_bin" -r '.layout.genericProfiles[], .layout.machineContracts[]' "$distribution")

while IFS= read -r relative; do
  if [[ ! -f "$share_root/$relative" ]]; then
    echo "portable distribution is missing declared license notice: $relative" >&2
    exit 1
  fi
done < <("$jq_bin" -r '.layout.thirdPartyLicenses[]' "$distribution")

# Reject every undeclared profile, including future development-only profiles.
"$jq_bin" -r '.layout.genericProfiles[] | split("/")[-1]' "$distribution" | LC_ALL=C sort >"$work_root/expected-profiles.txt"
for profile in "$profile_dir"/*.json; do basename "$profile"; done | LC_ALL=C sort >"$work_root/installed-profiles.txt"
if ! cmp -s "$work_root/expected-profiles.txt" "$work_root/installed-profiles.txt"; then
  echo "portable install profile inventory differs from its public contract" >&2
  exit 1
fi

if grep -R -F "$source_root" "$share_root" >"$work_root/source-path-leaks.txt" ||
   LC_ALL=C grep -a -F "$source_root" "$tool" >>"$work_root/source-path-leaks.txt"; then
  echo "portable install embeds its source repository path" >&2
  cat "$work_root/source-path-leaks.txt" >&2
  exit 1
fi

expected_version="$(tr -d '\r\n' <"$share_root/VERSION")"
minimum_version="$expected_version"
if [[ "$expected_version" == *-dev.* || "$expected_version" == *-rc.* ]]; then
  minimum_version="$("$jq_bin" -er --arg version "$expected_version" '
    select(.schemaVersion == "shin.parallel-development-policy.v1" and
           .developmentLane.coreVersion == $version and .cutover.automatic == false) |
    .stableLane.version | select(test("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"))
  ' "$source_root/docs/machine/shin-development-v1.json")"
fi
"$tool" --version >"$work_root/version.json"
"$jq_bin" -e --arg version "$expected_version" \
  '.ok == true and .version == $version and .generator_version == "shin-generator-v23-unicode-lexical"' \
  "$work_root/version.json" >/dev/null

project_root="$work_root/project"
runtime_profiles="$work_root/runtime-profiles"
cache_root="$work_root/cache"
out_root="$cache_root/portable-node"
marker="docs/machine/shin-project-adoption-v1.json"
mkdir -p "$project_root/src" "$project_root/docs/machine" "$runtime_profiles"
cp "$profile_dir"/*.json "$runtime_profiles/"
printf '%s\n' \
  '{"name":"portable-node-fixture","private":true,"scripts":{"check":"node --check src/account.js"}}' \
  >"$project_root/package.json"
printf '%s\n' \
  '# Portable Project' \
  '' \
  'The project validates a relocated Shin distribution.' \
  >"$project_root/README.md"
printf '%s\n' \
  'export function resolvePortableAccount(id) {' \
  '  return { id, source: "portable" };' \
  '}' \
  >"$project_root/src/account.js"
printf '%s\n' \
  '# Portable Account Contract' \
  '' \
  'This document is the source-owned portable account lookup contract.' \
  >"$project_root/docs/contract.md"

"$jq_bin" '
  .id = "portable-node" |
  .sourceLabel = "Portable Node permanent profile" |
  .bootstrapOnly = false |
  .agentStartPaths = [
    "AGENTS.md",
    "docs/machine/shin-project-adoption-v1.json",
    "README.md",
    "docs/contract.md"
  ] |
  .evalCases = [{
    id: "portable-account-contract",
    query: "portable account contract",
    mustPaths: ["docs/contract.md"],
    mustTopPaths: ["docs/contract.md"],
    mustTopN: 3,
    limit: 6,
    perDocLimit: 2
  }] |
  .packEvalCases = [{
    id: "portable-account-pack",
    request: {
      schema_version: "shin.task-request.v1",
      request_id: "portable-account-pack",
      task: {
        objective: "Load the portable account contract and implementation.",
        scope: [{id: "contract", query: "portable account contract"}],
        must: [{id: "implementation", query: "resolvePortableAccount function"}],
        should: []
      },
      budget: {
        max_evidence_bytes: 12000,
        max_records: 8,
        per_query_records: 4,
        per_document_records: 2
      }
    },
    mustPaths: ["docs/contract.md", "src/account.js"],
    mustNotPaths: [],
    expectedComplete: true,
    requireSerialParallelIdentity: true,
    maxEvidenceBytes: 12000,
    maxRecords: 8,
    maxCompactBytes: 18000
  }]
' "$profile_dir/generic-node.json" >"$runtime_profiles/portable-node.json"

context_epoch="portable-shin-v1.19-2026-08-18"
"$jq_bin" -n \
  --arg version "$minimum_version" \
  --arg epoch "$context_epoch" '
  {
    schemaVersion: "shin.project-adoption.v1",
    document: "portable-node-shin-project-adoption",
    status: "active",
    summary: "Portable fixture adopts Shin after the staged prefix is relocated.",
    project: {id: "portable-node", contextEpoch: $epoch, rootPolicy: "cli-root"},
    engine: {minimumVersion: $version, generatorIdentity: "shin-generator-v23-unicode-lexical"},
    profile: {
      id: "portable-node",
      registry: "explicit-profile-dir",
      kind: "permanent",
      genericBootstrapAllowed: false
    },
    index: {
      ownership: "tool-external",
      key: "portable-node",
      consumerWritesAllowed: false,
      requireCurrent: true
    },
    qualityGates: {
      profileCheckStrict: true,
      check: true,
      doctorStrict: true,
      evalStrict: true,
      packEvalStrict: true
    },
    agent: {
      entryPath: "AGENTS.md",
      contextEpochReferenceRequired: true,
      markerReferenceRequired: true,
      profileReferenceRequired: true
    }
  }
' >"$project_root/$marker"
printf '%s\n' \
  '# Portable Fixture Agent Contract' \
  '' \
  "Context epoch: \`$context_epoch\`." \
  '' \
  "Read \`$marker\` and use permanent Shin profile \`portable-node\`." \
  >"$project_root/AGENTS.md"

"$jq_bin" -n \
  --arg profile_dir "$runtime_profiles" \
  --arg project_root "$project_root" '
  {
    schemaVersion: "shin.profile-routing.v1",
    id: "portable-routing",
    routes: [{
      id: "portable-node",
      description: "Relocated portable fixture route",
      profile: "portable-node",
      profileDir: $profile_dir,
      root: $project_root,
      marker: "docs/machine/shin-project-adoption-v1.json",
      signals: ["portable account", "portable node"]
    }]
  }
' >"$work_root/profile-routing.json"
"$tool" suggest-profile --config "$work_root/profile-routing.json" \
  --from-root "$project_root" 'portable account contract' \
  >"$work_root/profile-suggestion.json"

"$tool" --root "$project_root" discover >"$work_root/discover.json"
"$jq_bin" -e '
  .ok == true and
  .bootstrap.primary_profile == "generic-node" and
  .bootstrap.project_mutation_required == false
' "$work_root/discover.json" >/dev/null

"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  adoption-plan --marker "$marker" --agent-entry AGENTS.md >"$work_root/adoption-plan.json"
"$tool" --profile-dir "$runtime_profiles" --root "$project_root" \
  adoption-check --marker "$marker" --strict >"$work_root/adoption.json"
if [[ "$expected_version" == *-dev.* || "$expected_version" == *-rc.* ]]; then
  cp "$project_root/$marker" "$work_root/valid-marker.json"
  for invalid_minimum in "$expected_version" "${expected_version%%-*}"; do
    "$jq_bin" --arg version "$invalid_minimum" '.engine.minimumVersion = $version' \
      "$work_root/valid-marker.json" >"$project_root/$marker"
    if "$tool" --profile-dir "$runtime_profiles" --root "$project_root" \
      adoption-check --marker "$marker" --strict >"$work_root/rejected-candidate-minimum.json"; then
      echo "development binary accepted an invalid or unreleased adoption minimum" >&2
      exit 1
    fi
    expected_code="engine-version-too-old"
    if [[ "$invalid_minimum" == "$expected_version" ]]; then
      expected_code="invalid-minimum-version"
    fi
    "$jq_bin" -e --arg code "$expected_code" --arg minimum "$invalid_minimum" '
      .ok == false and .engine.minimum_version == $minimum and
      any(.issues[]; .code == $code) and any(.issues[]; .code == "engine-version-too-old")' \
      "$work_root/rejected-candidate-minimum.json" >/dev/null
  done
  cp "$work_root/valid-marker.json" "$project_root/$marker"
fi
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  profile-check --strict >"$work_root/profile-check.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  profile-policy >"$work_root/profile-policy.json"
"$jq_bin" -e '
  .ok and .schema_version == "shin.profile-policy.v1" and
  .capabilities.read_only and (.capabilities.activation == false) and
  (.readiness_assessed == false) and .index_state == "not-assessed" and
  .fields.sourceFactRoots.origin == "profile" and
  (.revision.profile_config_hash | startswith("fnv1a64:"))
' "$work_root/profile-policy.json" >/dev/null
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" build >"$work_root/build.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" check >"$work_root/check.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" worktree-diff --strict >"$work_root/worktree-diff.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" doctor --strict >"$work_root/doctor.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" eval --strict >"$work_root/eval.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" pack-eval --strict >"$work_root/pack-eval.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" validate --marker "$marker" --strict >"$work_root/validate.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" validate --marker "$project_root/$marker" --strict --summary \
  >"$work_root/validate-summary.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" ready --marker "$project_root/$marker" \
  --routing-config "$work_root/profile-routing.json" --task 'portable account contract' \
  --strict >"$work_root/ready.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" brief 'portable account contract' --require-current >"$work_root/brief.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" inspect src/account.js --require-current >"$work_root/inspect.json"
"$jq_bin" '{schema_version:"shin.source-read-request.v1", snapshot_id,
  record_ids:[.records[] | select(.symbol_name == "resolvePortableAccount") | .id]}' \
  "$work_root/inspect.json" >"$work_root/read-request.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" read --input "$work_root/read-request.json" >"$work_root/read.json"
"$jq_bin" -e --rawfile source "$project_root/src/account.js" '
  .schema_version == "shin.source-read.v1" and .ok and .complete and
  .index_freshness.current and .requested_records == 2 and
  ([.evidence.records[].record_type] | sort) == ["ts-export", "ts-function"] and
  all(.evidence.records[]; .body_index == 0) and
  (.evidence.bodies | length) == 1 and .evidence.bodies[0].text == $source and
  (.evidence.omissions | length) == 0
' "$work_root/read.json" >/dev/null
"$jq_bin" '{schema_version:"shin.source-read-request.v2", snapshot_id,
  file_paths:[.path]}' "$work_root/inspect.json" >"$work_root/file-read-request.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" read --input "$work_root/file-read-request.json" >"$work_root/file-read.json"
"$jq_bin" -e --rawfile source "$project_root/src/account.js" '
  .schema_version == "shin.source-read.v2" and .ok and .complete and
  .index_freshness.current and .requested_files == 1 and
  (.evidence.files | length) == 1 and (.evidence.bodies | length) == 1 and
  .evidence.bodies[0].text == $source and (.evidence.omissions | length) == 0
' "$work_root/file-read.json" >/dev/null
"$jq_bin" '.packEvalCases[0].request' "$runtime_profiles/portable-node.json" \
  >"$work_root/task-request.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" pack --input "$work_root/task-request.json" --projection compact \
  --require-current --strict >"$work_root/pack.json"

write_project_fingerprint() {
  find "$project_root" -type f -print | LC_ALL=C sort | while IFS= read -r path; do
    shasum -a 256 "$path"
  done
}

mkdir -p "$work_root/external-run"
write_project_fingerprint >"$work_root/project-before-execution-evidence.txt"
"$jq_bin" -n '{schema_version:"shin.execution-input-request.v1",coverage:"declared-complete",selectors:[{path:"src/account.js",kind:"file"}]}' \
  >"$work_root/execution-input-request.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" receipt-scope --input "$work_root/execution-input-request.json" \
  --task-id portable-task --thread-id portable-thread --observed-at 1000 \
  >"$work_root/receipt-scope.json"
cp "$work_root/receipt-scope.json" "$work_root/external-run/execution-inputs.json"
printf '%s\n' \
  '<testsuite tests="1" failures="0" errors="0" skipped="0">' \
  '  <testcase classname="portable" name="relocated-import" time="0" />' \
  '</testsuite>' \
  >"$work_root/external-run/junit.xml"
"$jq_bin" -n \
  --arg report_path "$work_root/external-run/junit.xml" \
  --arg inputs_path "$work_root/external-run/execution-inputs.json" \
  '{schema_version:"shin.receipt-import-request.v1",format:"junit",report_path:$report_path,inputs_path:$inputs_path,
    receipt_id:"portable-receipt",producer:"portable-runner",started_at:1005,observed_at:1005,
    ttl_seconds:3600,command_fingerprint:"fnv1a64:0123456789abcdef",complete:true}' \
  >"$work_root/external-run/receipt-import-request.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" receipt-import --input "$work_root/external-run/receipt-import-request.json" \
  --task-id portable-task --thread-id portable-thread --observed-at 1010 \
  >"$work_root/receipt-import.json"
cp "$work_root/receipt-import.json" "$work_root/external-run/receipt-bundle.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" receipt-check --input "$work_root/external-run/receipt-bundle.json" \
  --task-id portable-task --thread-id portable-thread --observed-at 1010 \
  --trusted-producer portable-runner --require-current --strict \
  >"$work_root/receipt-check.json"
write_project_fingerprint >"$work_root/project-after-execution-evidence.txt"
cmp "$work_root/project-before-execution-evidence.txt" "$work_root/project-after-execution-evidence.txt"
"$jq_bin" -e '
  .schema_version == "shin.execution-inputs.v1" and
  .coverage == "declared-complete" and
  .binding.task_id == "portable-task" and
  .binding.thread_id == "portable-thread" and
  .selectors == [{path:"src/account.js",kind:"file"}] and
  .observations[0].observation == "observed"
' "$work_root/receipt-scope.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.receipt-bundle.v1" and .command == "receipt-import" and .ok and
  (.receipts | length) == 1 and .receipts[0].result == "pass" and
  .receipts[0].inputs.coverage == "declared-complete" and
  .receipts[0].report.format == "junit" and
  .receipts[0].report.payload_retained == false and
  (.receipts[0].report | has("report_path") | not)
' "$work_root/receipt-import.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.receipt-assessment.v1" and .command == "receipt-check" and
  .ok and .strict and .require_current and .index_current and
  (.receipts | length) == 1 and .receipts[0].passed_current and
  .receipts[0].input_coverage == "declared-complete" and
  .receipts[0].inputs_current and .receipts[0].narrowed_scope_used and
  .receipts[0].verification_level == "executed" and
  .receipts[0].report.payload_retained == false and
  .policy.read_only and (.policy.actions_executed | not) and
  (.policy.source_authority_upgraded | not)
' "$work_root/receipt-check.json" >/dev/null

"$jq_bin" '{schema_version:"shin.session-draft.v1", task_request:.,
  state:{current_slice:"Verify relocated session recovery",assertions:[],changed_paths:[],identifiers:[],next_actions:[]},
  receipts:[],vcs:null,ttl_seconds:3600}' "$work_root/task-request.json" >"$work_root/session-draft.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" checkpoint --input "$work_root/session-draft.json" \
  --task-id portable-task --thread-id portable-thread --observed-at 1000 --require-current \
  >"$work_root/session-checkpoint.json"
"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" resume --checkpoint "$work_root/session-checkpoint.json" \
  --task-id portable-task --thread-id portable-thread --observed-at 1010 --require-current \
  >"$work_root/session-resume.json"
"$jq_bin" -e '.schema_version=="shin.session-resume.v1" and .ok and .continuation_ready and
  .status=="current" and .continuation_pack.completion.complete and .pack_comparison.same_receipt' \
  "$work_root/session-resume.json" >/dev/null

"$tool" --profile portable-node --profile-dir "$runtime_profiles" --root "$project_root" \
  --out-dir "$out_root" impact --require-current --strict >"$work_root/impact.json"
"$jq_bin" -e '.schema_version=="shin.impact-pack.v1" and .ok and .index_current and
  .coverage.selection_complete and (.reread|length)==0 and .policy.read_only and
  (.policy.actions_executed|not)' "$work_root/impact.json" >/dev/null

cp -R "$out_root" "$cache_root/legacy-portable"
"$jq_bin" '.generator_version = "shin-generator-v13-evidence-provenance"' \
  "$cache_root/legacy-portable/docs/kb/generated/manifest.json" \
  >"$work_root/legacy-manifest.json"
mv "$work_root/legacy-manifest.json" \
  "$cache_root/legacy-portable/docs/kb/generated/manifest.json"
"$tool" index-list --cache-dir "$cache_root" >"$work_root/index-list-before.json"
"$tool" index-prune --cache-dir "$cache_root" --keep-key portable-node \
  >"$work_root/index-prune-dry.json"
"$tool" index-prune --cache-dir "$cache_root" --keep-key portable-node --apply --strict \
  >"$work_root/index-prune-apply.json"
"$tool" index-list --cache-dir "$cache_root" --strict >"$work_root/index-list.json"

for result in adoption-plan adoption profile-suggestion profile-check build check worktree-diff doctor eval pack-eval validate validate-summary ready brief pack receipt-import receipt-check index-list-before index-prune-dry index-prune-apply index-list; do
  "$jq_bin" -e '.ok == true' "$work_root/$result.json" >/dev/null
done
"$jq_bin" -e '
  .schema_version == "shin.profile-suggestion.v1" and .ok and
  .state == "current-route" and .recommendation.route_id == "portable-node" and
  .recommendation.profile == "portable-node" and
  (.policy.index_access | not) and (.policy.automatic_switch | not) and
  (.policy.project_mutation | not)
' "$work_root/profile-suggestion.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.worktree-diff.v1" and .ok and .current and
  .comparison.snapshot_integrity == "verified" and
  .comparison.git_required == false and .comparison.read_only == true and
  .summary.total_changes == 0
' "$work_root/worktree-diff.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.adoption-plan.v1" and
  .state == "adoption-check-ready" and
  .candidate_profile.id == "portable-node" and
  .candidate_profile.ready_for_marker_authoring == false and
  .readiness.adoption_check_ready == true and
  .readiness.adoption_validated == false and
  .project_mutation_performed == false and
  all(.commands[]; .writes_consumer == false)
' "$work_root/adoption-plan.json" >/dev/null
"$jq_bin" -e '
  any(.related_documents[]; . == "docs/contract.md") and
  .index_freshness.current == true
' "$work_root/brief.json" >/dev/null
"$jq_bin" -e '
  .completion.complete == true and
  any(.evidence[]; .location.path == "docs/contract.md") and
  any(.evidence[]; .location.path == "src/account.js")
' "$work_root/pack.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.validation.v1" and .ok and
  .summary.total == 7 and .summary.attempted == 7 and
  .summary.passed == 7 and .summary.failed == 0 and .summary.skipped == 0 and
  ([.gates[] | .ok] | all)
' "$work_root/validate.json" >/dev/null
"$jq_bin" -e --arg marker "$marker" '
  .schema_version == "shin.validation-summary.v2" and .projection == "summary" and
  .ok and .marker == $marker and .summary.passed == 7 and
  ([.gates[] | has("result") or has("error")] | any | not)
' "$work_root/validate-summary.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.agent-readiness.v1" and .ok and .ready and
  .alignment.routing.state == "current-route" and
  .alignment.adoption == {index_key:true, profile:true, required:true} and
  .summary.attempted == 4 and .summary.passed == 4 and
  .next_action == "brief-or-pack"
' "$work_root/ready.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.index-list.v1" and .ok and (.clean | not) and
  .summary.current == 1 and .summary.incompatible_generator == 1
' "$work_root/index-list-before.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.index-prune.v1" and .ok and (.apply | not) and
  .keep_keys == ["portable-node"] and .summary.candidates == 1 and
  .summary.pruned == 0 and .summary.remaining_candidates == 1
' "$work_root/index-prune-dry.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.index-prune.v1" and .ok and .clean and .apply and
  .summary.candidates == 1 and .summary.pruned == 1 and
  .summary.remaining_candidates == 0 and
  .actions == [{
    key: "legacy-portable",
    path: .actions[0].path,
    reason: "incompatible-generator",
    snapshot_id: .actions[0].snapshot_id,
    generator_version: "shin-generator-v13-evidence-provenance",
    snapshot_bytes: .actions[0].snapshot_bytes,
    apply_requested: true,
    applied: true,
    error: null
  }]
' "$work_root/index-prune-apply.json" >/dev/null
"$jq_bin" -e '
  .schema_version == "shin.index-list.v1" and .ok and .clean and
  .summary.current == 1 and .summary.incompatible_generator == 0 and
  .summary.empty_shell == 1
' "$work_root/index-list.json" >/dev/null
test -d "$out_root/docs/kb/generated"
test ! -e "$cache_root/legacy-portable/docs/kb/generated"

mkdir -p "$(dirname "$report")"
"$jq_bin" -n \
  --arg version "$expected_version" \
  --slurpfile version_result "$work_root/version.json" \
  --slurpfile discover "$work_root/discover.json" \
  --slurpfile adoption_plan "$work_root/adoption-plan.json" \
  --slurpfile adoption "$work_root/adoption.json" \
  --slurpfile profile_suggestion "$work_root/profile-suggestion.json" \
  --slurpfile profile_check "$work_root/profile-check.json" \
  --slurpfile profile_policy "$work_root/profile-policy.json" \
  --slurpfile worktree_diff "$work_root/worktree-diff.json" \
  --slurpfile doctor "$work_root/doctor.json" \
  --slurpfile eval "$work_root/eval.json" \
  --slurpfile pack_eval "$work_root/pack-eval.json" \
  --slurpfile validate "$work_root/validate.json" \
  --slurpfile validate_summary "$work_root/validate-summary.json" \
  --slurpfile ready "$work_root/ready.json" \
  --slurpfile brief "$work_root/brief.json" \
  --slurpfile inspection "$work_root/inspect.json" \
  --slurpfile source_read "$work_root/read.json" \
  --slurpfile pack "$work_root/pack.json" \
  --slurpfile receipt_scope "$work_root/receipt-scope.json" \
  --slurpfile receipt_import "$work_root/receipt-import.json" \
  --slurpfile receipt_check "$work_root/receipt-check.json" \
  --slurpfile session_checkpoint "$work_root/session-checkpoint.json" \
  --slurpfile session_resume "$work_root/session-resume.json" \
  --slurpfile impact "$work_root/impact.json" \
  --slurpfile index_list_before "$work_root/index-list-before.json" \
  --slurpfile index_prune_dry "$work_root/index-prune-dry.json" \
  --slurpfile index_prune_apply "$work_root/index-prune-apply.json" \
  --slurpfile index_list "$work_root/index-list.json" '
  {
    schema_version: "shin.portable-install-check.v1",
    tool: "shin-portable-install-gate",
    command: "check",
    ok: true,
    version: $version,
    stability: $version_result[0].stability,
    fixture_adoption_minimum: $adoption[0].engine.minimum_version,
    stable_release_certification: false,
    relocated: true,
    source_tree_required_after_install: false,
    integration_profiles_installed: false,
    generic_profile_count: 8,
    runtime: {
      version: $version_result[0].ok,
      discover: $discover[0].ok,
      adoption_plan: $adoption_plan[0].ok,
      adoption_check: $adoption[0].ok,
      profile_suggestion: $profile_suggestion[0].ok,
      profile_check: $profile_check[0].ok,
      profile_policy: $profile_policy[0].ok,
      worktree_diff: $worktree_diff[0].ok,
      doctor: $doctor[0].ok,
      eval: $eval[0].ok,
      pack_eval: $pack_eval[0].ok,
      validate: $validate[0].ok,
      validate_summary: $validate_summary[0].ok,
      ready: $ready[0].ready,
      brief: $brief[0].ok,
      inspect: $inspection[0].ok,
      read: ($source_read[0].ok and $source_read[0].complete),
      pack: $pack[0].ok,
      receipt_scope: ($receipt_scope[0].schema_version == "shin.execution-inputs.v1"),
      receipt_import: $receipt_import[0].ok,
      receipt_check: $receipt_check[0].ok,
      checkpoint: ($session_checkpoint[0].schema_version == "shin.session-checkpoint.v1"),
      resume: $session_resume[0].continuation_ready,
      impact: $impact[0].coverage.selection_complete,
      index_list: $index_list[0].clean,
      index_prune_dry_run: ($index_prune_dry[0].summary.candidates == 1),
      index_prune_apply: ($index_prune_apply[0].summary.pruned == 1)
    },
    snapshot_id: $pack[0].snapshot_id
  }
' >"$work_root/report.json"
mv "$work_root/report.json" "$report"
cat "$report"
