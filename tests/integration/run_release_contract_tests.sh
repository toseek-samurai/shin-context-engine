#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  echo "usage: run_release_contract_tests.sh <jq> <shin-context> <source-root> <report> [generated-manifest]" >&2
  exit 2
fi

jq_bin="$1"
tool="$2"
source_root="$3"
report="$4"

if [[ ! -x "$tool" ]]; then
  echo "shin-context binary is not executable: $tool" >&2
  exit 2
fi
if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for release-contract-check: $jq_bin" >&2
  exit 2
fi

version_file="$source_root/VERSION"
machine_manifest="$source_root/docs/machine/shin-context-engine.manifest.json"
generated_manifest="${5:-$source_root/docs/kb/generated/manifest.json}"
retrieval_evidence_contract="$source_root/docs/machine/retrieval-evidence-v1.json"
retrieval_observation_contract="$source_root/docs/machine/retrieval-observation-v1.json"
evidence_provenance_contract="$source_root/docs/machine/evidence-provenance-v1.json"
trust_boundary_contract="$source_root/docs/machine/trust-boundary-hardening-v1.json"
agent_context_pack_contract="$source_root/docs/machine/agent-context-pack-v1.json"
agent_context_pack_diff_contract="$source_root/docs/machine/agent-context-pack-diff-v1.json"
batch_retrieval_contract="$source_root/docs/machine/batch-retrieval-v1.json"
retrieval_diagnose_contract="$source_root/docs/machine/retrieval-diagnose-v1.json"
worktree_diff_contract="$source_root/docs/machine/worktree-diff-v1.json"
session_contract="$source_root/docs/machine/session-checkpoint-v1.json"
execution_evidence_contract="$source_root/docs/machine/execution-evidence-v1.json"
execution_receipt_contract="$source_root/docs/machine/execution-receipt-v1.json"
snapshot_diff_contract="$source_root/docs/machine/snapshot-diff-v1.json"
universality_contract="$source_root/docs/machine/universality-boundary-v1.json"
adoption_planner_contract="$source_root/docs/machine/adoption-planner-v1.json"
project_adoption_contract="$source_root/docs/machine/project-adoption-v1.json"
portable_distribution_contract="$source_root/docs/machine/portable-distribution-v1.json"
aggregate_validation_contract="$source_root/docs/machine/aggregate-validation-v1.json"
profile_routing_contract="$source_root/docs/machine/profile-routing-v1.json"
agent_readiness_contract="$source_root/docs/machine/agent-readiness-v1.json"
index_lifecycle_contract="$source_root/docs/machine/index-lifecycle-v1.json"
static_analysis_contract="$source_root/docs/machine/static-analysis-v1.json"
codex_skill="$source_root/integrations/codex/skills/shin-context/SKILL.md"
hermes_skill="$source_root/integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md"
expected_version="$(cat "$version_file")"
expected_version="${expected_version%$'\r'}"
release_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
candidate_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-(dev|rc)\.(0|[1-9][0-9]*)$'
expected_stability="stable"
if [[ "$expected_version" =~ $candidate_pattern ]]; then
  expected_stability="development"
elif [[ ! "$expected_version" =~ $release_pattern ]]; then
  echo "VERSION must be a canonical release or development candidate" >&2
  exit 2
fi

if grep -Fq "Current stable release" "$source_root/README.md"; then
  echo "README must not duplicate a stable-release declaration" >&2
  exit 1
fi
if grep -Eq 'shin-generator-v[0-9]+-' "$source_root/README.md"; then
  echo "README must not declare a literal generator identity" >&2
  exit 1
fi
for required_reference in '`VERSION`' 'docs/machine/shin-context-engine.manifest.json'; do
  if ! grep -Fq "$required_reference" "$source_root/README.md"; then
    echo "README must point to $required_reference" >&2
    exit 1
  fi
done
skill_minimum_version() {
  local file="$1"
  local directive="$2"
  local matches
  matches="$(sed -nE "s/^[[:space:]]*${directive} Shin v([0-9]+\\.[0-9]+\\.[0-9]+) or newer.*/\\1/p" "$file")"
  [[ -n "$matches" && "$matches" != *$'\n'* ]] || return 1
  printf '%s\n' "$matches"
}
version_at_least() {
  local actual="$1"
  local minimum="$2"
  [[ "$actual" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || return 1
  "$jq_bin" -en --arg actual "$actual" --arg minimum "$minimum" '
    def components: split(".") | map([length, .]);
    ($actual | components) >= ($minimum | components)
  ' >/dev/null
}
codex_minimum="$(skill_minimum_version "$codex_skill" 'Use')" || codex_minimum=""
hermes_minimum="$(skill_minimum_version "$hermes_skill" 'Require')" || hermes_minimum=""
if ! version_at_least "$codex_minimum" "2.0.0" ||
   ! version_at_least "$hermes_minimum" "2.0.0" ||
   ! grep -Fq '## Normal Work' "$codex_skill" ||
   ! grep -Fq 'shin_status' "$codex_skill" ||
   ! grep -Fq 'shin_query' "$codex_skill" ||
   ! grep -Fq 'shin-generator-v23-unicode-lexical' "$codex_skill" ||
   ! grep -Fq 'shin-generator-v23-unicode-lexical' "$hermes_skill"; then
  echo "integration skills must retain approved per-client minimums, managed 2.0 guidance, and the current Unicode generator" >&2
  exit 1
fi

work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-release-contract.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

bundle_files=()
while IFS= read -r relative; do
  [[ "$relative" != /* && "$relative" != *".."* && -f "$source_root/$relative" ]] || {
    echo "invalid or missing integration bundle" >&2; exit 2;
  }
  bundle_files+=("$source_root/$relative")
done < <("$jq_bin" -r '.integrationBundles[]' "$universality_contract")
if [[ ${#bundle_files[@]} -eq 0 ]]; then
  echo "release contract requires declared integration fixtures" >&2; exit 2
fi
"$jq_bin" -s '.' "${bundle_files[@]}" >"$work_root/integration-bundles.json"

candidate_policy="$work_root/no-development-policy.json"
printf '{}\n' >"$candidate_policy"
if [[ "$expected_stability" == development ]]; then
  candidate_policy="$source_root/docs/machine/shin-development-v1.json"
fi

"$tool" --version >"$work_root/version.json"
"$tool" help >"$work_root/help.json"
mkdir -p "$(dirname "$report")"

"$jq_bin" -n \
  --arg expected_version "$expected_version" \
  --arg expected_stability "$expected_stability" \
  --arg release_pattern "$release_pattern" \
  --slurpfile candidate_policy "$candidate_policy" \
  --slurpfile version "$work_root/version.json" \
  --slurpfile help "$work_root/help.json" \
  --slurpfile machine "$machine_manifest" \
  --slurpfile generated "$generated_manifest" \
  --slurpfile retrieval_evidence "$retrieval_evidence_contract" \
  --slurpfile retrieval_observation "$retrieval_observation_contract" \
  --slurpfile evidence_provenance "$evidence_provenance_contract" \
  --slurpfile trust_boundary "$trust_boundary_contract" \
  --slurpfile agent_context_pack "$agent_context_pack_contract" \
  --slurpfile agent_context_pack_diff "$agent_context_pack_diff_contract" \
  --slurpfile batch_retrieval "$batch_retrieval_contract" \
  --slurpfile retrieval_diagnose "$retrieval_diagnose_contract" \
  --slurpfile worktree_diff "$worktree_diff_contract" \
  --slurpfile session_contract "$session_contract" \
  --slurpfile execution_evidence "$execution_evidence_contract" \
  --slurpfile execution_receipt "$execution_receipt_contract" \
  --slurpfile snapshot_diff "$snapshot_diff_contract" \
  --slurpfile universality "$universality_contract" \
  --slurpfile adoption_planner "$adoption_planner_contract" \
  --slurpfile project_adoption "$project_adoption_contract" \
  --slurpfile portable_distribution "$portable_distribution_contract" \
  --slurpfile aggregate_validation "$aggregate_validation_contract" \
  --slurpfile profile_routing "$profile_routing_contract" \
  --slurpfile agent_readiness "$agent_readiness_contract" \
  --slurpfile index_lifecycle "$index_lifecycle_contract" \
  --slurpfile static_analysis "$static_analysis_contract" \
  --slurpfile integration_bundles "$work_root/integration-bundles.json" '
    def equality_check($id; $actual; $expected):
      {id: $id, ok: ($actual == $expected), actual: $actual, expected: $expected};
    def canonical_release:
      if type == "string" then test("\\A" + $release_pattern + "\\z") else false end;
    def canonical_generator:
      if type == "string" then
        test("\\Ashin-generator-v[1-9][0-9]*-[a-z0-9]+(-[a-z0-9]+)*\\z")
      else false end;
    # Compare canonical numeric components without shell integer overflow or
    # floating-point rounding for unusually large version components.
    def release_order: split(".") | map([length, .]);

    ($version[0]) as $version_result |
    ($help[0]) as $help_result |
    ($machine[0]) as $machine_manifest |
    ($generated[0]) as $generated_manifest |
    ($retrieval_evidence[0]) as $retrieval_evidence_contract |
    ($retrieval_observation[0]) as $retrieval_observation_contract |
    ($evidence_provenance[0]) as $evidence_provenance_contract |
    ($trust_boundary[0]) as $trust_boundary_contract |
    ($agent_context_pack[0]) as $agent_context_pack_contract |
    ($agent_context_pack_diff[0]) as $agent_context_pack_diff_contract |
    ($batch_retrieval[0]) as $batch_retrieval_contract |
    ($retrieval_diagnose[0]) as $retrieval_diagnose_contract |
    ($worktree_diff[0]) as $worktree_diff_contract |
    ($execution_evidence[0]) as $execution_evidence_contract |
    ($snapshot_diff[0]) as $snapshot_diff_contract |
    ($universality[0]) as $universality_contract |
    ($adoption_planner[0]) as $adoption_planner_contract |
    ($project_adoption[0]) as $project_adoption_contract |
    ($portable_distribution[0]) as $portable_distribution_contract |
    ($aggregate_validation[0]) as $aggregate_validation_contract |
    ($profile_routing[0]) as $profile_routing_contract |
    ($agent_readiness[0]) as $agent_readiness_contract |
    ($index_lifecycle[0]) as $index_lifecycle_contract |
    ($static_analysis[0]) as $static_analysis_contract |
    $integration_bundles[0] as $integration_bundles |
    (($help_result.commands // []) | map(.name) | sort) as $help_commands |
    (($machine_manifest.commandCatalog // []) | sort) as $manifest_commands |
    ($candidate_policy[0]) as $development_policy |
    ($expected_stability == "development") as $development |
    # Inherited metadata uses a historical baseline, not the installed delivery.
    # Binary/project identity and every behavioral check remain independent.
    (if $development then $development_policy.developmentLane.contractBaselineVersion else $expected_version end) as $contract_version |
    (if $development then $development_policy.developmentLane.contractBaselineGenerator else $version_result.generator_version end) as $contract_generator |
    (if $development then [
      equality_check("development-policy-single-document"; ($candidate_policy | length); 1),
      equality_check("development-policy-schema"; $development_policy.schemaVersion; "shin.parallel-development-policy.v1"),
      equality_check("development-policy-status"; $development_policy.status; "accepted"),
      equality_check("development-policy-core-version"; $development_policy.developmentLane.coreVersion; $expected_version),
      equality_check("development-policy-core-generator"; $development_policy.developmentLane.coreGenerator; $version_result.generator_version),
      equality_check("development-policy-separate-builds"; $development_policy.developmentLane.separateCheckoutAndBuilds; true),
      equality_check("development-policy-no-automatic-cutover"; $development_policy.cutover.automatic; false),
      equality_check("development-baseline-canonical"; ($contract_version | canonical_release); true),
      equality_check("development-delivery-canonical"; ($development_policy.stableLane.version | canonical_release); true),
      equality_check("development-delivery-generator-canonical"; ($development_policy.stableLane.generator | canonical_generator); true),
      equality_check("development-baseline-generator-canonical"; ($contract_generator | canonical_generator); true),
      equality_check("development-version-after-baseline";
        (if ($contract_version | canonical_release) then
          ($contract_version | release_order) < ($expected_version | split("-")[0] | release_order)
        else false end); true),
      equality_check("development-project-status"; $machine_manifest.project.status; "development"),
      equality_check("development-release-fields-present";
        ($machine_manifest.project.release | has("tag") and has("date")); true),
      equality_check("development-release-date"; $machine_manifest.project.release.date; null),
      equality_check("development-version-source"; $machine_manifest.project.release.versionSource; "VERSION"),
      equality_check("development-baseline-policy"; $machine_manifest.project.stableBaseline.policy; "docs/machine/shin-development-v1.json"),
      equality_check("development-baseline-version"; $machine_manifest.project.stableBaseline.version; $contract_version),
      equality_check("development-baseline-tag"; $machine_manifest.project.stableBaseline.tag; "v" + $contract_version),
      equality_check("development-baseline-generator"; $machine_manifest.project.stableBaseline.generator; $contract_generator)
    ] else [] end) as $lane_checks |
    [
      equality_check("version-result-schema"; $version_result.schema_version; "shin.version.v1"),
      equality_check("version-result-value"; $version_result.version; $expected_version),
      equality_check("version-result-stability"; $version_result.stability; $expected_stability),
      equality_check("version-generator-canonical"; ($version_result.generator_version | canonical_generator); true),
      equality_check("generated-json-decoder-library"; $version_result.generated_json_decoder.library; "simdjson"),
      equality_check("generated-json-decoder-version"; $version_result.generated_json_decoder.version; "4.6.10"),
      equality_check("generated-json-decoder-api"; $version_result.generated_json_decoder.api; "dom"),
      equality_check("generated-json-decoder-scope"; $version_result.generated_json_decoder.scope; "generated-jsonl-only"),
      equality_check("machine-project-version"; $machine_manifest.project.version; $expected_version),
      equality_check("machine-release-tag"; $machine_manifest.project.release.tag; if $development then null else "v" + $expected_version end),
      equality_check("machine-version-schema"; $machine_manifest.schemas.version; $version_result.schema_version),
      equality_check("generated-generator-version"; $generated_manifest.generator_version; $version_result.generator_version),
      equality_check("command-catalog"; $manifest_commands; $help_commands),
      equality_check("command-catalog-unique"; ($help_commands | unique | length); ($help_commands | length)),
      equality_check("version-command-documented"; ($machine_manifest.commands.version | type); "string"),
      equality_check("inspect-command-documented"; ($machine_manifest.commands.inspect | type); "string"),
      equality_check("read-command-documented"; ($machine_manifest.commands.read | type); "string"),
      equality_check("adoption-plan-command-documented"; ($machine_manifest.commands.adoptionPlan | type); "string"),
      equality_check("adoption-check-command-documented"; ($machine_manifest.commands.adoptionCheck | type); "string"),
      equality_check("validate-command-documented"; ($machine_manifest.commands.validate | type); "string"),
      equality_check("suggest-profile-command-documented"; ($machine_manifest.commands.suggestProfile | type); "string"),
      equality_check("ready-command-documented"; ($machine_manifest.commands.ready | type); "string"),
      equality_check("index-list-command-documented"; ($machine_manifest.commands.indexList | type); "string"),
      equality_check("index-prune-command-documented"; ($machine_manifest.commands.indexPrune | type); "string"),
      equality_check("pack-command-documented"; ($machine_manifest.commands.pack | type); "string"),
      equality_check("pack-diff-command-documented"; ($machine_manifest.commands.packDiff | type); "string"),
      equality_check("pack-bench-command-documented"; ($machine_manifest.commands.packBench | type); "string"),
      equality_check("pack-eval-command-documented"; ($machine_manifest.commands.packEval | type); "string"),
      equality_check("batch-command-documented"; ($machine_manifest.commands.batch | type); "string"),
      equality_check("diagnose-command-documented"; ($machine_manifest.commands.diagnose | type); "string"),
      equality_check("worktree-diff-command-documented"; ($machine_manifest.commands.worktreeDiff | type); "string"),
      equality_check("impact-command-documented"; ($machine_manifest.commands.impact | type); "string"),
      equality_check("impact-schema"; $machine_manifest.schemas.impactPack; "shin.impact-pack.v1"),
      equality_check("source-syntax-runtime"; ($machine_manifest.dependencies | map(select(.id == "tree-sitter") | .version)); ["0.27.0"]),
      equality_check("checkpoint-command-documented"; ($machine_manifest.commands.checkpoint | type); "string"),
      equality_check("resume-command-documented"; ($machine_manifest.commands.resume | type); "string"),
      equality_check("receipt-scope-command-documented"; ($machine_manifest.commands.receiptScope | type); "string"),
      equality_check("receipt-import-command-documented"; ($machine_manifest.commands.receiptImport | type); "string"),
      equality_check("receipt-check-command-documented"; ($machine_manifest.commands.receiptCheck | type); "string"),
      equality_check("impact-receipts-flags-documented"; ($machine_manifest.commands.impact | contains("--receipts") and contains("--task-id") and contains("--thread-id")); true),
      equality_check("resume-trusted-producer-flag-documented"; ($machine_manifest.commands.resume | contains("--trusted-producer")); true),
      equality_check("session-checkpoint-schema"; $machine_manifest.schemas.sessionCheckpoint; "shin.session-checkpoint.v1"),
      equality_check("session-resume-schema"; $machine_manifest.schemas.sessionResume; "shin.session-resume.v1"),
      equality_check("execution-input-request-schema"; $machine_manifest.schemas.executionInputRequest; "shin.execution-input-request.v1"),
      equality_check("execution-inputs-schema"; $machine_manifest.schemas.executionInputs; "shin.execution-inputs.v1"),
      equality_check("receipt-import-request-schema"; $machine_manifest.schemas.receiptImportRequest; "shin.receipt-import-request.v1"),
      equality_check("receipt-bundle-schema"; $machine_manifest.schemas.receiptBundle; "shin.receipt-bundle.v1"),
      equality_check("receipt-assessment-schema"; $machine_manifest.schemas.receiptAssessment; "shin.receipt-assessment.v1"),
      equality_check("execution-evidence-schema"; $machine_manifest.schemas.executionEvidence; "shin.execution-evidence.v1"),
      equality_check("execution-receipt-schema"; $machine_manifest.schemas.executionReceipt; "shin.execution-receipt.v1"),
      equality_check("session-contract-version"; $session_contract[0].release.productVersion; $contract_version),
      equality_check("session-contract-read-only"; $session_contract[0].commands.readOnly; true),
      equality_check("session-contract-auto-actions"; $session_contract[0].commands.automaticActions; false),
      equality_check("execution-receipt-contract-version"; $execution_receipt[0].release.productVersion; $contract_version),
      equality_check("execution-evidence-contract-path"; $machine_manifest.executionEvidenceContract; "docs/machine/execution-evidence-v1.json"),
      equality_check("execution-evidence-contract-schema"; $execution_evidence_contract.schemaVersion; "shin.execution-evidence.v1"),
      equality_check("execution-evidence-contract-status"; $execution_evidence_contract.status; "accepted"),
      equality_check("execution-evidence-contract-version"; $execution_evidence_contract.release.productVersion; $contract_version),
      equality_check("execution-evidence-contract-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/execution-evidence-v1.json"); true),
      equality_check("session-contract-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/session-checkpoint-v1.json"); true),
      equality_check("execution-receipt-contract-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/execution-receipt-v1.json"); true),
      equality_check("snapshot-diff-command-documented"; ($machine_manifest.commands.snapshotDiff | type); "string"),
      equality_check("federated-eval-command-documented"; ($machine_manifest.commands.federatedEval | type); "string"),
      equality_check("batch-schema-documented"; $machine_manifest.schemas.batch; "shin.batch.v1"),
      equality_check("task-request-schema-documented"; $machine_manifest.schemas.taskRequest; "shin.task-request.v1"),
      equality_check("brief-compact-schema-documented"; $machine_manifest.schemas.briefCompact; "shin.context-brief-compact.v1"),
      equality_check("agent-context-pack-schema-documented"; $machine_manifest.schemas.agentContextPack; "shin.agent-context-pack.v1"),
      equality_check("agent-context-pack-compact-schema-documented"; $machine_manifest.schemas.agentContextPackCompact; "shin.agent-context-pack-compact.v1"),
      equality_check("agent-context-pack-diff-schema-documented"; $machine_manifest.schemas.agentContextPackDiff; "shin.agent-context-pack-diff.v1"),
      equality_check("context-pack-receipt-schema-documented"; $machine_manifest.schemas.contextPackReceipt; "shin.context-pack-receipt.v1"),
      equality_check("pack-telemetry-schema-documented"; $machine_manifest.schemas.packTelemetry; "shin.pack-telemetry.v1"),
      equality_check("pack-bench-schema-documented"; $machine_manifest.schemas.packBench; "shin.pack-bench.v1"),
      equality_check("pack-eval-schema-documented"; $machine_manifest.schemas.packEval; "shin.pack-eval.v1"),
      equality_check("validation-schema-documented"; $machine_manifest.schemas.validation; "shin.validation.v1"),
      equality_check("validation-summary-schema-documented"; $machine_manifest.schemas.validationSummary; "shin.validation-summary.v2"),
      equality_check("eval-failures-schema-documented"; $machine_manifest.schemas.evalFailures; "shin.eval-failures.v1"),
      equality_check("pack-eval-failures-schema-documented"; $machine_manifest.schemas.packEvalFailures; "shin.pack-eval-failures.v1"),
      equality_check("aggregate-validation-contract-schema-documented"; $machine_manifest.schemas.aggregateValidationContract; "shin.aggregate-validation-contract.v1"),
      equality_check("profile-suggestion-schema-documented"; $machine_manifest.schemas.profileSuggestion; "shin.profile-suggestion.v1"),
      equality_check("profile-routing-contract-schema-documented"; $machine_manifest.schemas.profileRoutingContract; "shin.profile-routing-contract.v1"),
      equality_check("agent-readiness-schema-documented"; $machine_manifest.schemas.agentReadiness; "shin.agent-readiness.v1"),
      equality_check("agent-readiness-contract-schema-documented"; $machine_manifest.schemas.agentReadinessContract; "shin.agent-readiness-contract.v1"),
      equality_check("index-list-schema-documented"; $machine_manifest.schemas.indexList; "shin.index-list.v1"),
      equality_check("index-prune-schema-documented"; $machine_manifest.schemas.indexPrune; "shin.index-prune.v1"),
      equality_check("index-lifecycle-contract-schema-documented"; $machine_manifest.schemas.indexLifecycleContract; "shin.index-lifecycle-contract.v1"),
      equality_check("diagnose-schema-documented"; $machine_manifest.schemas.diagnose; "shin.diagnose.v1"),
      equality_check("worktree-diff-schema-documented"; $machine_manifest.schemas.worktreeDiff; "shin.worktree-diff.v1"),
      equality_check("worktree-diff-contract-schema-documented"; $machine_manifest.schemas.worktreeDiffContract; "shin.worktree-diff-contract.v1"),
      equality_check("evidence-provenance-contract-schema-documented"; $machine_manifest.schemas.evidenceProvenanceContract; "shin.evidence-provenance-contract.v1"),
      equality_check("snapshot-diff-schema-documented"; $machine_manifest.schemas.snapshotDiff; "shin.snapshot-diff.v1"),
      equality_check("federated-eval-schema-documented"; $machine_manifest.schemas.federatedEval; "shin.federated-eval.v1"),
      equality_check("retrieval-evidence-contract-path"; $machine_manifest.retrievalEvidenceContract; "docs/machine/retrieval-evidence-v1.json"),
      equality_check("retrieval-evidence-contract-schema"; $retrieval_evidence_contract.schemaVersion; "shin.retrieval-evidence-contract.v1"),
      equality_check("retrieval-evidence-contract-status"; $retrieval_evidence_contract.status; "accepted"),
      equality_check("retrieval-evidence-coverage-policy"; $retrieval_evidence_contract.queryCoverage.policy; "returned-evidence-and-planned-document-coverage-v1"),
      equality_check("retrieval-ranking-policy"; $retrieval_evidence_contract.rankedRetrieval.policy; "global-evidence-order-unique-path-v1"),
      equality_check("retrieval-global-document-budget-policy";
                     ($retrieval_evidence_contract.rankedRetrieval.selectionRule | contains("One per-document limit applies across all types"));
                     true),
      equality_check("retrieval-owner-scope-policy"; $retrieval_evidence_contract.documentOwnerScope.policy; "document-owner-scope-v1"),
      equality_check("retrieval-owner-scope-mode"; $retrieval_evidence_contract.documentOwnerScope.mode; "owner-scoped"),
      equality_check("retrieval-directory-scope-policy"; $retrieval_evidence_contract.directoryOwnerScope.policy; "directory-owner-scope-v1"),
      equality_check("retrieval-directory-scope-mode"; $retrieval_evidence_contract.directoryOwnerScope.mode; "directory-scoped"),
      equality_check("retrieval-live-source-scan-mode"; $retrieval_evidence_contract.liveSourceScan.mode; "source-scan"),
      equality_check("retrieval-observation-contract-path"; $machine_manifest.retrievalObservationContract; "docs/machine/retrieval-observation-v1.json"),
      equality_check("retrieval-observation-contract-schema"; $retrieval_observation_contract.schemaVersion; "shin.retrieval-observation.v1"),
      equality_check("retrieval-observation-contract-status"; $retrieval_observation_contract.status; "accepted"),
      equality_check("retrieval-observation-contract-version"; $retrieval_observation_contract.release.productVersion; $contract_version),
      equality_check("retrieval-observation-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/retrieval-observation-v1.json"); true),
      equality_check("retrieval-brief-compact-schema"; $retrieval_evidence_contract.briefProjections.compact.schema; "shin.context-brief-compact.v1"),
      equality_check("retrieval-section-authority-policy"; $retrieval_evidence_contract.sectionAuthority.rankingPolicy; "current-authority-weight-v1"),
      equality_check("evidence-provenance-contract-path"; $machine_manifest.evidenceProvenanceContract; "docs/machine/evidence-provenance-v1.json"),
      equality_check("evidence-provenance-contract-schema"; $evidence_provenance_contract.schemaVersion; "shin.evidence-provenance-contract.v1"),
      equality_check("evidence-provenance-contract-status"; $evidence_provenance_contract.status; "accepted"),
      equality_check("evidence-provenance-contract-version"; $evidence_provenance_contract.release.productVersion; $contract_version),
      equality_check("evidence-provenance-contract-generator"; $evidence_provenance_contract.release.generator; $contract_generator),
      equality_check("evidence-provenance-v125-generator-cutover";
                     ($evidence_provenance_contract.release.compatibility | contains("requires an explicit generator-v17 rebuild"));
                     true),
      equality_check("evidence-provenance-generated-fields"; $evidence_provenance_contract.generatedStores.fields; ["evidence_kind", "verification_level"]),
      equality_check("evidence-provenance-ranking-neutral"; $evidence_provenance_contract.retrieval.ranking; "The fields are not sparse terms, scoring inputs, boosts, or penalties in v1.17. Existing status and authority ranking remains unchanged."),
      equality_check("agent-context-pack-contract-path"; $machine_manifest.agentContextPackContract; "docs/machine/agent-context-pack-v1.json"),
      equality_check("agent-context-pack-contract-schema"; $agent_context_pack_contract.schemaVersion; "shin.agent-context-pack-contract.v1"),
      equality_check("agent-context-pack-contract-status"; $agent_context_pack_contract.status; "accepted"),
      equality_check("agent-context-pack-v125-generator-cutover";
                     ($agent_context_pack_contract.release.compatibility | contains("requires an explicit generator-v17 rebuild"));
                     true),
      equality_check("agent-context-pack-byte-policy"; $agent_context_pack_contract.budget.bytePolicy; "compact-json-evidence-array-bytes-v1"),
      equality_check("agent-context-pack-selection-policy"; $agent_context_pack_contract.evidence.selectionPolicy; "replacement-aware-requirement-seed-ranked-fill-v4"),
      equality_check("agent-context-pack-global-document-budget";
                     ($agent_context_pack_contract.evidence.perDocumentBudget | contains("one global selected-record ceiling"));
                     true),
      equality_check("agent-context-pack-canonical-record-deduplication";
                     $agent_context_pack_contract.evidence.deduplicationPolicy;
                     "canonical-record-v1"),
      equality_check("agent-context-pack-pressure-policy"; $agent_context_pack_contract.budget.pressure.policy; "fixed-utilization-thresholds-v1"),
      equality_check("agent-context-pack-pressure-thresholds"; $agent_context_pack_contract.budget.pressure.thresholdsBasisPoints; {warning: 8500, critical: 9500}),
      equality_check("agent-context-pack-pressure-advisory"; $agent_context_pack_contract.budget.pressure.strictBoundary; "Pressure is advisory. warning or critical alone never changes ok, completion, receipt, or --strict exit behavior."),
      equality_check("agent-context-pack-default-projection"; $agent_context_pack_contract.projections.default; "full"),
      equality_check("agent-context-pack-compact-schema"; $agent_context_pack_contract.projections.compact.schema; "shin.agent-context-pack-compact.v1"),
      equality_check("agent-context-pack-telemetry-schema"; $agent_context_pack_contract.telemetry.schema; "shin.pack-telemetry.v1"),
      equality_check("agent-context-pack-eval-schema"; $agent_context_pack_contract.profileQuality.schema; "shin.pack-eval.v1"),
      equality_check("agent-context-pack-bench-schema"; $agent_context_pack_contract.benchmark.schema; "shin.pack-bench.v1"),
      equality_check("agent-context-pack-diff-contract-path"; $machine_manifest.agentContextPackDiffContract; "docs/machine/agent-context-pack-diff-v1.json"),
      equality_check("agent-context-pack-diff-contract-schema"; $agent_context_pack_diff_contract.schemaVersion; "shin.agent-context-pack-diff-contract.v1"),
      equality_check("agent-context-pack-diff-contract-status"; $agent_context_pack_diff_contract.status; "accepted"),
      equality_check("agent-context-pack-diff-output-schema"; $agent_context_pack_diff_contract.command.outputSchema; "shin.agent-context-pack-diff.v1"),
      equality_check("batch-contract-path"; $machine_manifest.batchRetrievalContract; "docs/machine/batch-retrieval-v1.json"),
      equality_check("batch-contract-schema"; $batch_retrieval_contract.schemaVersion; "shin.batch-retrieval-contract.v1"),
      equality_check("diagnose-contract-path"; $machine_manifest.retrievalDiagnoseContract; "docs/machine/retrieval-diagnose-v1.json"),
      equality_check("diagnose-contract-schema"; $retrieval_diagnose_contract.schemaVersion; "shin.retrieval-diagnose-contract.v1"),
      equality_check("worktree-diff-contract-path"; $machine_manifest.worktreeDiffContract; "docs/machine/worktree-diff-v1.json"),
      equality_check("worktree-diff-contract-schema"; $worktree_diff_contract.schemaVersion; "shin.worktree-diff-contract.v1"),
      equality_check("worktree-diff-contract-status"; $worktree_diff_contract.status; "accepted"),
      equality_check("worktree-diff-contract-version"; $worktree_diff_contract.release.productVersion; $contract_version),
      equality_check("worktree-diff-output-schema"; $worktree_diff_contract.command.outputSchema; "shin.worktree-diff.v1"),
      equality_check("worktree-diff-git-independent"; $worktree_diff_contract.command.gitRequired; false),
      equality_check("worktree-diff-read-only"; [$worktree_diff_contract.command.indexMutationAllowed, $worktree_diff_contract.command.sourceMutationAllowed]; [false, false]),
      equality_check("snapshot-diff-contract-path"; $machine_manifest.snapshotDiffContract; "docs/machine/snapshot-diff-v1.json"),
      equality_check("snapshot-diff-contract-schema"; $snapshot_diff_contract.schemaVersion; "shin.snapshot-diff-contract.v1"),
      equality_check("universality-contract-path"; $machine_manifest.universalityBoundaryContract; "docs/machine/universality-boundary-v1.json"),
      equality_check("universality-contract-schema"; $universality_contract.schemaVersion; "shin.universality-boundary.v1"),
      equality_check("universality-contract-status"; $universality_contract.status; "accepted"),
      equality_check("universality-contract-release"; $universality_contract.release; $contract_version),
      equality_check("universality-schema-documented"; $machine_manifest.schemas.universalityBoundary; "shin.universality-boundary.v1"),
      equality_check("universality-check-schema-documented"; $machine_manifest.schemas.universalityCheck; "shin.universality-check.v1"),
      equality_check("adoption-plan-schema-documented"; $machine_manifest.schemas.adoptionPlan; "shin.adoption-plan.v1"),
      equality_check("adoption-planner-contract-path"; $machine_manifest.adoptionPlannerContract; "docs/machine/adoption-planner-v1.json"),
      equality_check("adoption-planner-contract-schema"; $adoption_planner_contract.schemaVersion; "shin.adoption-planner-contract.v1"),
      equality_check("adoption-planner-contract-status"; $adoption_planner_contract.status; "accepted"),
      equality_check("adoption-planner-contract-release"; $adoption_planner_contract.release; $contract_version),
      equality_check("adoption-planner-output-schema"; $adoption_planner_contract.command.outputSchema; "shin.adoption-plan.v1"),
      equality_check("adoption-planner-read-only"; $adoption_planner_contract.output.projectMutationPerformed; false),
      equality_check("adoption-planner-output-template";
                     $adoption_planner_contract.command.externalOutputTemplate;
                     "Every generated-index command uses <EXTERNAL_OUTPUT_BASE>/<INDEX_KEY>. Bootstrap may choose a temporary key; permanent adoption must substitute the exact validated marker.index.key."),
      equality_check("project-adoption-contract-path"; $machine_manifest.projectAdoptionContract; "docs/machine/project-adoption-v1.json"),
      equality_check("project-adoption-contract-schema"; $project_adoption_contract.schemaVersion; "shin.project-adoption-contract.v1"),
      equality_check("project-adoption-contract-status"; $project_adoption_contract.status; "accepted"),
      equality_check("project-adoption-contract-release"; $project_adoption_contract.release; $contract_version),
      equality_check("project-adoption-marker-schema-documented"; $machine_manifest.schemas.projectAdoption; "shin.project-adoption.v1"),
      equality_check("project-adoption-check-schema-documented"; $machine_manifest.schemas.projectAdoptionCheck; "shin.project-adoption-check.v1"),
      equality_check("adoption-canonical-output-template";
                     $machine_manifest.adoptionIntegrationClosure.canonicalOutputTemplate;
                     "<external-output-base>/<marker.index.key>"),
      equality_check("adoption-same-snapshot-required";
                     $machine_manifest.adoptionIntegrationClosure.sameSnapshotRequired;
                     true),
      equality_check("adoption-project-gate-complete";
                     $machine_manifest.adoptionIntegrationClosure.requiredProjectGate;
                     ["adoption-check --strict", "profile-check --strict", "build", "check", "doctor --strict", "eval --strict", "pack-eval --strict"]),
      equality_check("project-adoption-project-gate-documented";
                     $project_adoption_contract.integrationClosure.singleProjectGate | type;
                     "string"),
      equality_check("portable-distribution-contract-path"; $machine_manifest.portableDistributionContract; "docs/machine/portable-distribution-v1.json"),
      equality_check("portable-distribution-contract-schema"; $portable_distribution_contract.schemaVersion; "shin.portable-distribution.v1"),
      equality_check("portable-distribution-contract-status"; $portable_distribution_contract.status; "accepted"),
      equality_check("portable-distribution-contract-release"; $portable_distribution_contract.release; $contract_version),
      equality_check("portable-install-schema-documented"; $machine_manifest.schemas.portableInstallCheck; "shin.portable-install-check.v1"),
      equality_check("aggregate-validation-contract-path"; $machine_manifest.aggregateValidationContract; "docs/machine/aggregate-validation-v1.json"),
      equality_check("aggregate-validation-contract-schema"; $aggregate_validation_contract.schemaVersion; "shin.aggregate-validation-contract.v1"),
      equality_check("aggregate-validation-contract-status"; $aggregate_validation_contract.status; "accepted"),
      equality_check("aggregate-validation-contract-release"; $aggregate_validation_contract.release; $contract_version),
      equality_check("aggregate-validation-output-schema"; $aggregate_validation_contract.command.outputSchema; "shin.validation.v1"),
      equality_check("aggregate-validation-summary-schema"; $aggregate_validation_contract.output.summaryProjection.schema; "shin.validation-summary.v2"),
      equality_check("aggregate-validation-summary-failures-bounded"; $aggregate_validation_contract.output.summaryProjection.failedGatePayloads; "bounded-failed-only"),
      equality_check("aggregate-validation-summary-budget"; $aggregate_validation_contract.output.summaryProjection.envelopeBudgetBytes; 65536),
      equality_check("aggregate-validation-summary-gate-budget"; $aggregate_validation_contract.output.summaryProjection.gateDetailBudgetBytes; 6144),
      equality_check("aggregate-validation-complete-gate-budget";
                     ($aggregate_validation_contract.output.summaryProjection.truncationPolicy |
                      contains("complete failed-gate result")); true),
      equality_check("aggregate-validation-eval-failure-schema"; $aggregate_validation_contract.output.summaryProjection.evalProjection; "shin.eval-failures.v1"),
      equality_check("aggregate-validation-pack-failure-schema"; $aggregate_validation_contract.output.summaryProjection.packEvalProjection; "shin.pack-eval-failures.v1"),
      equality_check("aggregate-validation-failure-budget-policy"; $aggregate_validation_contract.output.summaryProjection.failureProjections.policy; "whole-record-byte-budget-with-header-v2"),
      equality_check("aggregate-validation-eval-failure-budget"; $aggregate_validation_contract.output.summaryProjection.failureProjections.evalBudgetBytes; 24576),
      equality_check("aggregate-validation-pack-failure-budget"; $aggregate_validation_contract.output.summaryProjection.failureProjections.packEvalBudgetBytes; 16384),
      equality_check("aggregate-validation-required-failure-header";
                     $aggregate_validation_contract.output.summaryProjection.failureProjections.requiredHeaderFields;
                     ["diagnostic_id", "detail_level", "id", "failure_reasons", "primary_path", "details_omitted"]),
      equality_check("aggregate-validation-no-mutation"; $aggregate_validation_contract.command.mutationAllowed; false),
      equality_check("aggregate-validation-no-fail-fast"; $aggregate_validation_contract.execution.failFast; false),
      equality_check("profile-routing-contract-path"; $machine_manifest.profileRoutingContract; "docs/machine/profile-routing-v1.json"),
      equality_check("profile-routing-contract-schema"; $profile_routing_contract.schemaVersion; "shin.profile-routing-contract.v1"),
      equality_check("profile-routing-contract-status"; $profile_routing_contract.status; "accepted"),
      equality_check("profile-routing-contract-version"; $profile_routing_contract.release.productVersion; $contract_version),
      equality_check("profile-routing-output-schema"; $profile_routing_contract.command.outputSchema; "shin.profile-suggestion.v1"),
      equality_check("profile-routing-read-only"; [$profile_routing_contract.command.indexAccess, $profile_routing_contract.command.projectMutationAllowed]; [false, false]),
      equality_check("profile-routing-no-auto-switch"; $profile_routing_contract.command.automaticSwitchAllowed; false),
      equality_check("profile-routing-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/profile-routing-v1.json"); true),
      equality_check("agent-readiness-contract-path"; $machine_manifest.agentReadinessContract; "docs/machine/agent-readiness-v1.json"),
      equality_check("agent-readiness-contract-schema"; $agent_readiness_contract.schemaVersion; "shin.agent-readiness-contract.v1"),
      equality_check("agent-readiness-contract-status"; $agent_readiness_contract.status; "accepted"),
      equality_check("agent-readiness-contract-version"; $agent_readiness_contract.release.productVersion; $contract_version),
      equality_check("agent-readiness-output-schema"; $agent_readiness_contract.command.outputSchema; "shin.agent-readiness.v1"),
      equality_check("agent-readiness-no-build-or-mutation"; [$agent_readiness_contract.command.buildIncluded, $agent_readiness_contract.command.snapshotMutationAllowed, $agent_readiness_contract.command.consumerMutationAllowed]; [false, false, false]),
      equality_check("agent-readiness-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/agent-readiness-v1.json"); true),
      equality_check("index-lifecycle-contract-path"; $machine_manifest.indexLifecycleContract; "docs/machine/index-lifecycle-v1.json"),
      equality_check("index-lifecycle-contract-schema"; $index_lifecycle_contract.schemaVersion; "shin.index-lifecycle-contract.v1"),
      equality_check("index-lifecycle-contract-status"; $index_lifecycle_contract.status; "accepted"),
      equality_check("index-lifecycle-contract-version"; $index_lifecycle_contract.release.productVersion; $contract_version),
      equality_check("index-lifecycle-list-schema"; $index_lifecycle_contract.commands.list.outputSchema; "shin.index-list.v1"),
      equality_check("index-lifecycle-prune-schema"; $index_lifecycle_contract.commands.prune.outputSchema; "shin.index-prune.v1"),
      equality_check("index-lifecycle-dry-run-default"; $index_lifecycle_contract.commands.prune.dryRunDefault; true),
      equality_check("index-lifecycle-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/index-lifecycle-v1.json"); true),
      equality_check("trust-boundary-contract-path"; $machine_manifest.trustBoundaryHardeningContract; "docs/machine/trust-boundary-hardening-v1.json"),
      equality_check("trust-boundary-contract-schema"; $trust_boundary_contract.schemaVersion; "shin.trust-boundary-hardening.v1"),
      equality_check("trust-boundary-contract-version"; $trust_boundary_contract.version; $contract_version),
      equality_check("trust-boundary-contract-generator"; $trust_boundary_contract.generator; $contract_generator),
      equality_check("trust-boundary-invariant-contract";
                     ($trust_boundary_contract.invariants | contains([
                       "Coverage means transported retrieval support, not truth or product authority.",
                       "No read-only command creates or repairs index, lock or cache files.",
                       "Missing or malformed cache state causes read-only hash verification, not trust escalation.",
                       "No automatic command execution, consumer-specific rule or automatic compaction hook."
                     ]));
                     true),
      equality_check("trust-boundary-scope-contract";
                     ($trust_boundary_contract.scope | contains([
                       "Canonical-record-only Pack deduplication; every match remains bound to the full transported record and exact verification range.",
                       "Existing-only read locks; optional freshness cache is prepared by explicit publication, never repaired by retrieval."
                     ]));
                     true),
      equality_check("trust-boundary-rebuild-policy";
                     ($trust_boundary_contract.compatibility | contains("Generator-v16 publications must be rebuilt explicitly"));
                     true),
      equality_check("trust-boundary-installed"; any($portable_distribution_contract.layout.machineContracts[]; . == "contracts/trust-boundary-hardening-v1.json"); true),
      equality_check("static-analysis-contract-path"; $machine_manifest.staticAnalysisContract; "docs/machine/static-analysis-v1.json"),
      equality_check("static-analysis-contract-schema"; $static_analysis_contract.schemaVersion; "shin.static-analysis-contract.v1"),
      equality_check("static-analysis-contract-status"; $static_analysis_contract.status; "accepted"),
      equality_check("static-analysis-contract-version"; $static_analysis_contract.release.productVersion; $contract_version),
      equality_check("static-analysis-warnings-as-errors"; $static_analysis_contract.policy.warningsAsErrors; true),
      equality_check("static-analysis-no-auto-fix"; $static_analysis_contract.policy.automaticFixes; false),
      equality_check("static-analysis-third-party-excluded"; any($static_analysis_contract.scope.excluded[]; . == "third_party/**"); true),
      equality_check("static-analysis-versioned-tool-discovery";
                     ($static_analysis_contract.toolchain.executableDiscovery |
                      contains("version-suffixed")); true),
      equality_check("static-analysis-release-gate"; $static_analysis_contract.commands.releaseGate; "make static-analysis-check"),
      equality_check("simdjson-version-documented"; ($machine_manifest.dependencies | map(select(.id == "simdjson") | .version)); ["4.6.10"]),
      equality_check("simdjson-runtime-independent"; ($machine_manifest.dependencies | map(select(.id == "simdjson") | .runtimeDependency)); [false]),
      equality_check("pugixml-version-documented"; ($machine_manifest.dependencies | map(select(.id == "pugixml") | .version)); ["1.16"]),
      equality_check("pugixml-upstream-tag"; ($machine_manifest.dependencies | map(select(.id == "pugixml") | .upstreamTag)); ["v1.16"]),
      equality_check("pugixml-upstream-commit"; ($machine_manifest.dependencies | map(select(.id == "pugixml") | .upstreamCommit)); ["c8033ce9d039e7f9d134877c363397b3cfe20816"]),
      equality_check("pugixml-license"; ($machine_manifest.dependencies | map(select(.id == "pugixml") | .license)); ["MIT"]),
      equality_check("pugixml-runtime-independent"; ($machine_manifest.dependencies | map(select(.id == "pugixml") | .runtimeDependency)); [false]),
      equality_check("execution-source-inventory"; $machine_manifest.build.sources | contains(["src/execution_commands.cpp", "src/execution_inputs.cpp", "src/execution_io.cpp", "src/execution_receipts.cpp", "src/execution_report.cpp", "src/execution_report_junit.cpp", "src/execution_report_playwright.cpp"]); true),
      equality_check("execution-report-parser-dependency-inventory"; $machine_manifest.build.sources | contains(["third_party/pugixml/pugixml.cpp"]); true),
      equality_check("execution-source-ownership"; $machine_manifest.sourceOwnership.executionEvidence.implementation | contains(["src/execution_commands.cpp", "src/execution_inputs.cpp", "src/execution_io.cpp", "src/execution_receipts.cpp", "src/execution_report.cpp", "src/execution_report_junit.cpp", "src/execution_report_playwright.cpp"]); true),
      equality_check("pugixml-source-ownership"; $machine_manifest.sourceOwnership.pugixml.implementation; "third_party/pugixml/pugixml.cpp"),
      equality_check("pugixml-license-metadata"; $machine_manifest.sourceOwnership.pugixml.metadata; "third_party/pugixml/UPSTREAM.json"),
      equality_check("portable-installed-profile-set";
                     ($portable_distribution_contract.layout.genericProfiles | map(split("/")[-1] | sub("\\.json$"; "")) | sort);
                     ($universality_contract.core.profiles | map(select(.kind == "bootstrap") | .id) | sort)),
      equality_check("portable-integration-profiles-disabled"; $portable_distribution_contract.integrationPolicy.installedByDefault; false),
      equality_check("integration-bundle-schema-documented"; $machine_manifest.schemas.integrationBundle; "shin.integration-bundle.v1"),
      equality_check("integration-bundle-paths"; ($machine_manifest.integrationBundles | sort); ($universality_contract.integrationBundles | sort)),
      equality_check("integration-bundle-schemas"; ($integration_bundles | map(.schemaVersion) | unique); ["shin.integration-bundle.v1"]),
      equality_check("integration-bundles-not-runtime-dependencies"; all($integration_bundles[]; .coreRuntimeDependency == false); true),
      equality_check("integration-bundles-not-release-dependencies"; all($integration_bundles[]; .coreReleaseDependency == false); true),
      equality_check("portable-release-route"; $universality_contract.releaseRoutes.portable.release; "make release-check"),
      equality_check("ecosystem-release-owner"; $universality_contract.releaseRoutes.ecosystem.owner; "consumer"),
      equality_check("ecosystem-not-core-release-dependency"; $universality_contract.releaseRoutes.ecosystem.coreReleaseDependency; false)
    ] + $lane_checks | . as $checks |
    {
      schema_version: "shin.release-check.v1",
      tool: "shin-release-contract",
      command: "check",
      ok: all($checks[]; .ok),
      version: $expected_version,
      stability: $expected_stability,
      contract_version: $contract_version,
      contract_generator: $contract_generator,
      runtime_generator: $version_result.generator_version,
      generator_policy: "explicit-generator-identities-v1",
      stable_delivery: (if $development then {
        version: $development_policy.stableLane.version,
        generator: $development_policy.stableLane.generator,
        verification: "declared-metadata-not-live-installation"
      } else null end),
      version_policy: (if $development then "candidate-stable-baseline" else "stable" end),
      stable_release_metadata_validated: (($development | not) and all($checks[]; .ok)),
      checks: $checks
    }
  ' >"$work_root/report.json"

mv "$work_root/report.json" "$report"
cat "$report"
"$jq_bin" -e '.ok == true' "$report" >/dev/null
