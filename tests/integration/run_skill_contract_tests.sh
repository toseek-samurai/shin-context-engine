#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run_skill_contract_tests.sh <source-root>" >&2
  exit 2
fi

source_root="$1"
skill="$source_root/integrations/codex/skills/shin-context"
skill_md="$skill/SKILL.md"
agent_yaml="$skill/agents/openai.yaml"

require_text() {
  local file="$1"
  local expected="$2"
  local -a owners=("$file")
  if [[ "${file##*/}" == SKILL.md && -d "${file%/*}/references" ]]; then
    owners+=("${file%/*}"/references/*.md)
  fi
  if ! grep -Fq "$expected" "${owners[@]}"; then
    echo "expected '$expected' in $file" >&2
    exit 1
  fi
}

if [[ ! -f "$skill_md" || ! -f "$agent_yaml" ]]; then
  echo "canonical Shin Codex skill is incomplete" >&2
  exit 1
fi

if grep -Fq 'TODO' "$skill_md"; then
  echo "Shin Codex skill contains an unresolved TODO" >&2
  exit 1
fi

if [[ "$(wc -l <"$skill_md")" -gt 120 ]]; then
  echo "Shin Codex skill entry exceeds the 120-line routing limit" >&2
  exit 1
fi

require_text "$skill_md" 'name: shin-context'
require_text "$skill_md" 'before broad repository scans'
require_text "$skill_md" 'Use Shin v2.0.0 or newer'
require_text "$skill_md" 'shin-generator-v24-objective-cpp'
for native_tool in shin_status shin_query shin_inspect shin_read shin_help; do
  grep -Fq "$native_tool" "$skill_md"
done
if grep -Eq '1\.27\.|generator-v20|Legacy Adopted Routes' "$skill_md"; then
  echo 'Native skill still routes ordinary work to a retired release' >&2
  exit 1
fi
require_text "$skill/references/trust-boundaries.md" 'release-approved source policy'
require_text "$skill/references/trust-boundaries.md" 'same canonical external index'
for workflow in "$skill_md" "$source_root/integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md" "$source_root/integrations/mcp/skills/shin-context/SKILL.md"; do
  require_text "$workflow" 'automatic-project-text'
  require_text "$workflow" 'bounded-text-fallback'
  require_text "$workflow" 'shin_projects'
  require_text "$workflow" 'shin_status'
  require_text "$workflow" 'shin_query'
done
require_text "$skill/references/setup-and-adoption.md" 'one pattern'
require_text "$skill/references/setup-and-adoption.md" 'Project `.gitignore` files apply'
require_text "$skill/references/setup-and-adoption.md" '`!/local-notes/handbook.md`'
require_text "$skill/references/setup-and-adoption.md" 'last matching Shin rule wins'
require_text "$skill_md" 'worktree-diff'
require_text "$skill_md" 'Use `impact` before rebuilding'
require_text "$skill/references/impact.md" 'historical snapshot ranges'
require_text "$skill/references/impact.md" 'not a compiler call graph'
require_text "$skill_md" 'This command is Git-independent and read-only.'
require_text "$skill_md" 'does not overlay changed files into retrieval'
require_text "$skill_md" 'ready --marker <marker-path> --strict'
require_text "$skill_md" 'Require `ready=true`'
require_text "$skill_md" 'It never builds, repairs, switches profiles'
require_text "$skill_md" 'Do not manually build a managed index'
require_text "$skill_md" 'Ordinary `rg`, `jq` and direct source reads remain valid'
require_text "$skill_md" 'authority_level'
require_text "$skill_md" 'evidence_kind'
require_text "$skill_md" 'verification_level'
require_text "$skill_md" '`implementation-source` means current extracted source bytes, not deployed'
require_text "$skill_md" '`test-source` with `source-observed` means a test definition was'
require_text "$skill_md" 'Only a trusted'
require_text "$skill_md" 'shin.task-request.v1'
require_text "$skill_md" 'pack --input <task-request.json> --projection compact --require-current --strict'
require_text "$skill_md" 'completion.complete'
require_text "$skill_md" 'completion.coverage_complete'
require_text "$skill_md" 'selected_query_coverage'
require_text "$skill_md" 'require_full_coverage'
require_text "$skill_md" 'min_covered_groups'
require_text "$skill_md" 'verification_paths'
for workflow in "$skill_md" "$source_root/integrations/hermes/plugins/shin-context/skills/workflow/SKILL.md"; do
  require_text "$workflow" 'A heading-only Markdown range is a container, not its contents.'
  require_text "$workflow" 'same-or-higher-level'
  require_text "$workflow" 'use a structured parser'
  require_text "$workflow" 'additional source evidence and cost'
  require_text "$workflow" 'original single-call gate'
done
require_text "$skill_md" 'Add `--telemetry` only for an explicit performance investigation'
require_text "$skill_md" 'budget.pressure'
require_text "$skill_md" 'pack-eval --strict'
require_text "$skill_md" 'shin.batch-request.v1'
require_text "$skill_md" "diagnose '<task query>' --expected-path"
require_text "$skill_md" 'snapshot_id'
require_text "$skill_md" 'query_coverage.groups'
require_text "$skill_md" 'queryVariants'
require_text "$skill_md" "suggest-profile --config <routing-config> --from-root <consumer-root> '<task query>'"
require_text "$skill_md" 'For `ambiguous` or `no-match`, do not guess'
require_text "$skill_md" "federated-brief --config <federation.json> '<task query>' --require-current"
require_text "$skill_md" "federated-eval --config <federation.json> --suite <federation.eval.json> --strict"
require_text "$skill_md" 'Raw scores are member-local'
require_text "$skill_md" 'index-list --cache-dir <external-output-base> --strict'
require_text "$skill_md" 'index-prune --cache-dir <external-output-base> --keep-key <active-index-key>'
require_text "$skill_md" 'The first `index-prune` is a dry-run.'
require_text "$skill_md" 'Use `--prune-unreferenced` only when every active key'
require_text "$skill_md" 'snapshot-diff --from <old-output> --to <new-output>'
require_text "$agent_yaml" 'display_name: "Shin Context"'
require_text "$agent_yaml" 'default_prompt: "Use $shin-context'

if grep -Eq '/Users/[^/]+/|/home/[^/]+/' "$skill_md"; then
  echo "Shin Codex skill contains a machine-specific home path" >&2
  exit 1
fi

"${PYTHON:-python3}" "$source_root/tools/check_agent_guidance.py" "$source_root"
"${PYTHON:-python3}" "$source_root/tests/integration/run_agent_guidance_tests.py" "$source_root"

echo "Shin Codex skill contract tests passed."
