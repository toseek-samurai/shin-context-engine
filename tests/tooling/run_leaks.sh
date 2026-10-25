#!/usr/bin/env bash
set -euo pipefail

tool="${1:?shin-context binary is required}"
repo_root="${2:?repository root is required}"
leaks_root="${3:?leaks working directory is required}"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Apple leaks requires macOS; leak gate skipped."
  exit 0
fi

if ! command -v leaks >/dev/null 2>&1; then
  echo "Apple leaks is unavailable; leak gate skipped."
  exit 0
fi

if ! command -v codesign >/dev/null 2>&1 ||
  ! command -v plutil >/dev/null 2>&1 ||
  ! command -v jq >/dev/null 2>&1; then
  echo "Apple leak gate dependencies are unavailable; leak gate skipped."
  exit 0
fi

output_root="$leaks_root/shin"
log_root="$leaks_root/logs"
leaks_tool="$leaks_root/shin-context"

mkdir -p "$leaks_root" "$output_root" "$log_root"
cd "$repo_root"

if [[ "$tool" == "$leaks_tool" ]]; then
  echo "Leak gate requires a copy separate from the release binary." >&2
  exit 1
fi

entitlements_dir="$(mktemp -d "${TMPDIR:-/tmp}/shin-context-leaks.XXXXXX")"
cleanup() {
  rm -rf "$entitlements_dir"
}
trap cleanup EXIT

entitlements="$entitlements_dir/entitlements.plist"
plutil -create xml1 "$entitlements"
plutil -insert 'com\.apple\.security\.get-task-allow' -bool YES "$entitlements"

rm -f "$leaks_tool"
cp "$tool" "$leaks_tool"
chmod u+w "$leaks_tool"
codesign --force --sign - --identifier shin-context-leaks --timestamp=none \
  --entitlements "$entitlements" "$leaks_tool"
codesign --verify --strict "$leaks_tool"

run_leaks_check() {
  local label="$1"
  shift
  local log="$log_root/$label.txt"
  local status=0
  MallocStackLogging=1 leaks --quiet --atExit -- "$@" >"$log" 2>&1 || status=$?
  if grep -Fq 'not debuggable' "$log"; then
    echo "Apple leaks cannot inspect child processes in this environment; leak gate skipped."
    return 2
  fi
  if [[ "$status" -ne 0 ]]; then
    cat "$log" >&2
    return "$status"
  fi
  if ! grep -Fq '0 leaks for 0 total leaked bytes' "$log"; then
    cat "$log" >&2
    echo "Apple leaks did not emit a complete zero-leak result." >&2
    return 1
  fi
}

if run_leaks_check build "$leaks_tool" --profile shin --profile-dir profiles \
  --out-dir "$output_root" build; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then
    exit 0
  fi
  exit "$status"
fi

if run_leaks_check bench "$leaks_tool" --profile shin --profile-dir profiles \
  --out-dir "$output_root" bench "source facts" --iterations 3 --build-iterations 1; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then
    exit 0
  fi
  exit "$status"
fi

pack_request="$leaks_root/self-agent-operations-pack.json"
jq -e '.packEvalCases[] | select(.id == "self-agent-operations-pack") | .request' \
  profiles/shin.json >"$pack_request"

if run_leaks_check pack-eval "$leaks_tool" --profile shin --profile-dir profiles \
  --out-dir "$output_root" pack-eval --strict; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then
    exit 0
  fi
  exit "$status"
fi

if run_leaks_check pack-bench "$leaks_tool" --profile shin --profile-dir profiles \
  --out-dir "$output_root" pack-bench --input "$pack_request" --projection compact \
  --iterations 3 --warmup 1 --require-current --strict; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then
    exit 0
  fi
  exit "$status"
fi

session_draft="$entitlements_dir/session-draft.json"
session_checkpoint="$entitlements_dir/session-checkpoint.json"
jq '{schema_version:"shin.session-draft.v1",task_request:.,
  state:{current_slice:"Verify session lifetime",assertions:[],changed_paths:[],identifiers:[],next_actions:[]},
  receipts:[],vcs:null,ttl_seconds:3600}' "$pack_request" >"$session_draft"
session_route=("$leaks_tool" --profile shin --profile-dir profiles --out-dir "$output_root")
session_binding=(--task-id leak-test --thread-id leak-thread --observed-at 1000 --require-current)
"${session_route[@]}" checkpoint --input "$session_draft" "${session_binding[@]}" >"$session_checkpoint"
for mode in checkpoint resume; do
  if [[ "$mode" == checkpoint ]]; then
    session_args=(checkpoint --input "$session_draft")
  else
    session_args=(resume --checkpoint "$session_checkpoint")
  fi
  if run_leaks_check "session-$mode" "${session_route[@]}" "${session_args[@]}" "${session_binding[@]}"; then
    :
  else
    status=$?
    if [[ "$status" -eq 2 ]]; then exit 0; fi
    exit "$status"
  fi
done

syntax_route=("$leaks_tool" --profile member-ranges --profile-dir "$repo_root/tests/fixtures/member-ranges/profiles"
  --root "$repo_root/tests/fixtures/member-ranges/root" --out-dir "$leaks_root/source-syntax")
if run_leaks_check source-syntax "${syntax_route[@]}" build; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then exit 0; fi
  exit "$status"
fi

impact_root="$entitlements_dir/impact-root"
cp -R "$repo_root/tests/fixtures/impact/root" "$impact_root"
impact_route=("$leaks_tool" --profile impact --profile-dir "$repo_root/tests/fixtures/impact/profiles"
  --root "$impact_root" --out-dir "$entitlements_dir/impact-index")
"${impact_route[@]}" build >/dev/null
sed 's/value + 1/value + 2/' "$impact_root/src/PriceService.ts" >"$entitlements_dir/changed.ts"
mv "$entitlements_dir/changed.ts" "$impact_root/src/PriceService.ts"
if run_leaks_check impact "${impact_route[@]}" impact; then
  :
else
  status=$?
  if [[ "$status" -eq 2 ]]; then exit 0; fi
  exit "$status"
fi

echo "Apple leaks gate passed for build, retrieval, Pack, checkpoint, resume, syntax, and impact (eight probes)."
