#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
  echo "usage: run_perf_gate.sh <jq> <budgets> <perf-dir> <report> [core]" >&2
  exit 2
fi

jq_bin="$1"
budgets="$2"
perf_dir="$3"
report="$4"
scope="${5:-core}"

if ! command -v "$jq_bin" >/dev/null 2>&1; then
  echo "jq is required for perf-check: $jq_bin" >&2
  exit 2
fi

case "$scope" in
  core)
    profile_ids=(shin)
    ;;
  *)
    echo "unsupported perf gate scope: $scope" >&2
    exit 2
    ;;
esac

profile_files=()
for result in "${profile_ids[@]}"; do
  if [[ ! -f "$perf_dir/$result.json" ]]; then
    echo "missing benchmark result: $perf_dir/$result.json" >&2
    exit 2
  fi
  profile_files+=("$perf_dir/$result.json")
done
if [[ ! -f "$perf_dir/agent-context-pack.json" ]]; then
  echo "missing benchmark result: $perf_dir/agent-context-pack.json" >&2
  exit 2
fi

work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-perf-gate.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT
mkdir -p "$(dirname "$report")"
"$jq_bin" -s 'reduce .[] as $run ({}; .[$run.profile] = $run)' \
  "${profile_files[@]}" >"$work_root/profiles.json"
expected_profiles_json="$(printf '%s\n' "${profile_ids[@]}" \
  | "$jq_bin" -Rsc 'split("\n") | map(select(length > 0))')"

"$jq_bin" -n \
  --arg scope "$scope" \
  --argjson expected_profiles "$expected_profiles_json" \
  --slurpfile budgets "$budgets" \
  --slurpfile profile_runs "$work_root/profiles.json" \
  --slurpfile agent_context_pack "$perf_dir/agent-context-pack.json" '
    def equality_check($id; $actual; $expected):
      {id: $id, ok: ($actual == $expected), actual: $actual, expected: $expected};

    def minimum_check($id; $actual; $minimum):
      {id: $id, ok: (($actual | type) == "number" and $actual >= $minimum), actual: $actual, minimum: $minimum};

    def maximum_check($id; $actual; $maximum):
      {id: $id, ok: (($actual | type) == "number" and $actual <= $maximum), actual: $actual, maximum: $maximum};

    def metric_check($metrics; $entry):
      (($metrics // []) | map(select(.name == $entry.key)) | first | .latency_ms.p50) as $actual |
      {
        id: ("p50-" + $entry.key),
        metric: $entry.key,
        ok: (($actual | type) == "number" and $actual <= $entry.value),
        actual_p50_ms: $actual,
        maximum_p50_ms: $entry.value
      };

    def evaluate($profile; $run; $budget):
      ([
        equality_check("result-schema"; $run.schema_version; "shin.perf.v1"),
        equality_check("result-command"; $run.command; "bench"),
        equality_check("result-ok"; $run.ok; true),
        equality_check("profile"; $run.profile; $profile),
        equality_check("query"; $run.query; $budget.query),
        minimum_check("iterations"; $run.iterations; $budget.minimumIterations),
        minimum_check("build-iterations"; $run.build_iterations; $budget.minimumBuildIterations),
        equality_check("metric-budget-count"; (($budget.maxP50Ms // {}) | length); 4)
      ] + (($budget.maxP50Ms // {}) | to_entries | map(metric_check($run.metrics; .)))) as $checks |
      {
        profile: $profile,
        ok: all($checks[]; .ok),
        reference_p50_ms: $budget.referenceP50Ms,
        checks: $checks
      };

    def pack_metric_check($run; $entry):
      ($run.phase_latency_ms[$entry.key].p50) as $actual |
      {
        id: ("p50-" + $entry.key),
        metric: $entry.key,
        ok: (($actual | type) == "number" and $actual <= $entry.value),
        actual_p50_ms: $actual,
        maximum_p50_ms: $entry.value
      };

    def evaluate_pack($run; $budget):
      ([
        equality_check("result-schema"; $run.schema_version; "shin.pack-bench.v1"),
        equality_check("result-command"; $run.command; "pack-bench"),
        equality_check("result-ok"; $run.ok; true),
        equality_check("profile"; $run.profile; $budget.profile),
        equality_check("request-id"; $run.request_id; $budget.requestId),
        equality_check("projection"; $run.projection; $budget.projection),
        equality_check("require-current"; $run.require_current; true),
        equality_check("strict"; $run.strict; true),
        equality_check("receipt-stable"; $run.receipt_stable; true),
        equality_check("complete"; $run.complete; true),
        equality_check("completion-stable"; $run.completion_stable; true),
        minimum_check("iterations"; $run.iterations; $budget.minimumIterations),
        minimum_check("warmup"; $run.warmup; $budget.minimumWarmup),
        maximum_check("evidence-bytes"; $run.evidence_bytes; $budget.maxEvidenceBytes),
        maximum_check("output-bytes"; $run.output_bytes.max; $budget.maxOutputBytes),
        maximum_check("process-peak-rss-bytes"; $run.process_peak_rss_bytes; $budget.maxPeakRssBytes),
        equality_check("metric-budget-count"; (($budget.maxP50Ms // {}) | length); 6)
      ] + (($budget.maxP50Ms // {}) | to_entries | map(pack_metric_check($run; .)))) as $checks |
      {
        profile: $budget.profile,
        request_id: $budget.requestId,
        ok: all($checks[]; .ok),
        reference_p50_ms: $budget.referenceP50Ms,
        checks: $checks
      };

    ($budgets[0]) as $budget |
    ($profile_runs[0]) as $runs |
    [$expected_profiles[] as $profile |
      evaluate($profile; $runs[$profile]; $budget.profiles[$profile])
    ] as $profiles |
    evaluate_pack($agent_context_pack[0]; $budget.agentContextPack) as $pack |
    {
      schema_version: "shin.perf-gate.v1",
      tool: "shin-perf-gate",
      command: "check",
      scope: $scope,
      ok: ($budget.schemaVersion == "shin.perf-budgets.v1" and all($profiles[]; .ok) and $pack.ok),
      budget_schema_version: $budget.schemaVersion,
      policy: $budget.policy.semantics,
      profiles: $profiles,
      agent_context_pack: $pack
    }
  ' >"$work_root/report.json"

mv "$work_root/report.json" "$report"
cat "$report"
"$jq_bin" -e '.ok == true' "$report" >/dev/null
