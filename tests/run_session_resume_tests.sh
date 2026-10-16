#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_session_resume_tests.sh <jq> <shin-context> <source-root>" >&2
  exit 2
fi
jq_bin="$1"
tool="$2"
source_root="$3"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-session.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT
project="$work_root/project"
out="$work_root/cache"
mkdir -p "$project" "$out"
cp -R "$source_root/tests/fixtures/basic/root/." "$project/"
profiles="$source_root/tests/fixtures/basic/profiles"

shin() {
  "$tool" --profile fixture --profile-dir "$profiles" --root "$project" --out-dir "$out" "$@"
}
check_json() {
  if ! "$jq_bin" -e "$2" "$1" >/dev/null; then
    echo "Session assertion failed: $2" >&2
    cat "$1" >&2
    exit 1
  fi
}
reject() {
  local target="$1"
  shift
  if "$@" >"$target" 2>&1; then
    echo "Expected rejection: $*" >&2
    exit 1
  fi
}
capture() {
  shin checkpoint --input "$work_root/draft.json" --task-id task-a --thread-id thread-a --observed-at 1000 "$@"
}
resume() {
  shin resume --checkpoint "$work_root/checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1020 "$@"
}

shin build >"$work_root/build.json"
"$jq_bin" '{schema_version:"shin.session-draft.v1", task_request:.packEvalCases[0].request,
  state:{current_slice:"Complete fixture recovery", assertions:[
    {id:"authority",kind:"invariant",text:"Host owns external authority.",paths:["docs/architecture.md"],receipt_ids:[]}
  ],changed_paths:["docs/architecture.md"],identifiers:["triggerWidget"],
    next_actions:[{id:"next",text:"Read the current contract and finish the fixture."}]},
  receipts:[],vcs:null,ttl_seconds:3600}' "$profiles/fixture.json" >"$work_root/draft.json"
capture --require-current >"$work_root/baseline.json"
check_json "$work_root/baseline.json" '.schema_version == "shin.session-checkpoint.v1" and .baseline.index_current and .baseline.observation_complete and (.tracked_sources|length)>0'

"$jq_bin" --slurpfile checkpoint "$work_root/baseline.json" '
  .receipts=[{schema_version:"shin.execution-receipt.v1",receipt_id:"test-1",kind:"test",producer:"fixture-runner",
    observed_at:1000,ttl_seconds:60,result:"pass",command_fingerprint:"fnv1a64:0000000000000000",
    scope:($checkpoint[0].binding + {snapshot_id:$checkpoint[0].baseline.snapshot_id,worktree_id:$checkpoint[0].baseline.worktree_id})}] |
  .state.assertions += [{id:"gate",kind:"completed",text:"Fixture test passed.",paths:["docs/architecture.md"],receipt_ids:["test-1"]}]
' "$work_root/draft.json" >"$work_root/with-receipt.json"
mv "$work_root/with-receipt.json" "$work_root/draft.json"
capture --require-current --jobs 1 >"$work_root/checkpoint.json"
capture --require-current --jobs 4 >"$work_root/checkpoint-parallel.json"
cmp "$work_root/checkpoint.json" "$work_root/checkpoint-parallel.json"
resume --require-current --jobs 1 >"$work_root/untrusted.json"
check_json "$work_root/untrusted.json" '.ok and .status=="current" and .continuation_ready and .continuation_pack.completion.complete and .continuation_pack.index_freshness.fully_verified and (.receipts[0].usable|not) and .receipts[0].verification_level=="producer-asserted" and all(.state.assertions[]; .verification_level=="session-asserted" and (.claim_verified|not))'
resume --require-current --trusted-producer fixture-runner --jobs 1 >"$work_root/current.json"
resume --require-current --trusted-producer fixture-runner --jobs 4 >"$work_root/current-parallel.json"
cmp "$work_root/current.json" "$work_root/current-parallel.json"
check_json "$work_root/current.json" '.receipts[0].passed_current and .receipts[0].verification_level=="executed" and .state.assertions[1].receipts_current and .pack_comparison.same_evidence and .pack_comparison.same_receipt and (.policy.actions_executed|not) and (.policy.source_authority_upgraded|not)'

shin resume --checkpoint "$work_root/checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1060 --trusted-producer fixture-runner >"$work_root/receipt-expired.json"
check_json "$work_root/receipt-expired.json" '.checkpoint_age=="current" and .receipts[0].age=="expired" and (.receipts[0].usable|not) and (.state.assertions[1].receipts_current|not) and .state.assertions[1].references_current and .state.assertions[1].review_required'
shin resume --checkpoint "$work_root/checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 4600 --trusted-producer fixture-runner >"$work_root/expired.json"
check_json "$work_root/expired.json" '.status=="expired" and (.continuation_ready|not) and all(.state.assertions[];.review_required)'
shin resume --checkpoint "$work_root/checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 999 --trusted-producer fixture-runner >"$work_root/future.json"
check_json "$work_root/future.json" '.status=="future" and .receipts[0].age=="future" and (.receipts[0].usable|not)'
resume --task 'Different objective' >"$work_root/task-wording.json"
check_json "$work_root/task-wording.json" '.task_wording_changed and (.continuation_ready|not) and .review_required'

for binding in task thread; do
  task_id=task-a
  thread_id=thread-a
  if [[ "$binding" == task ]]; then task_id=task-b; else thread_id=thread-b; fi
  reject "$work_root/$binding-mismatch.json" shin resume --checkpoint "$work_root/checkpoint.json" --task-id "$task_id" --thread-id "$thread_id" --observed-at 1020
  check_json "$work_root/$binding-mismatch.json" '.status=="mismatched" and (.ok|not) and .continuation_pack==null and (has("state")|not)'
done
mkdir "$work_root/other-project"
reject "$work_root/root-mismatch.json" "$tool" --profile fixture --profile-dir "$profiles" --root "$work_root/other-project" --out-dir "$out" resume --checkpoint "$work_root/checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1020
check_json "$work_root/root-mismatch.json" '.status=="mismatched" and .binding_differences==["root"] and (has("state")|not)'

printf '\nChanged current contract.\n' >>"$project/docs/architecture.md"
reject "$work_root/stale.json" resume --require-current --trusted-producer fixture-runner
check_json "$work_root/stale.json" '(.ok|not) and .status=="drifted" and (.index_current|not) and .continuation_pack==null and .checkpoint_drift.changed_paths==1 and (.receipts[0].scope_current|not) and .next_actions[0].next_command.argv[-1]=="impact" and (.next_actions[0].next_command.index_mutation|not) and .next_actions[1].next_command.argv[-1]=="build" and (.next_actions[1].next_command.shell|not)'
resume >"$work_root/stale-soft.json"
check_json "$work_root/stale-soft.json" '.ok and (.index_current|not) and .continuation_pack==null'
reject "$work_root/stale-capture.json" capture --require-current
capture >"$work_root/dirty-checkpoint.json"
shin resume --checkpoint "$work_root/dirty-checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1020 >"$work_root/dirty-resume.json"
check_json "$work_root/dirty-resume.json" '.same_worktree and .checkpoint_drift.changed_paths==0 and .published_drift.summary.total_changes==1 and (.index_current|not)'
shin build >"$work_root/rebuild.json"
resume --require-current --trusted-producer fixture-runner >"$work_root/rebuilt.json"
check_json "$work_root/rebuilt.json" '.ok and .index_current and .status=="drifted" and .review_required and .continuation_pack.completion.complete and (.pack_comparison.same_receipt|not) and (.receipts[0].passed_current|not)'

rm "$project/docs/architecture.md"
resume >"$work_root/removed.json"
check_json "$work_root/removed.json" 'any(.checkpoint_drift.paths[];.path=="docs/architecture.md" and .current.observation=="missing" and .status=="changed")'
cp "$source_root/tests/fixtures/basic/root/docs/architecture.md" "$work_root/outside.md"
ln -s "$work_root/outside.md" "$project/docs/architecture.md"
resume >"$work_root/source-symlink.json"
check_json "$work_root/source-symlink.json" 'any(.checkpoint_drift.paths[];.path=="docs/architecture.md" and .current.observation=="unobserved" and .status=="unverified")'
rm "$project/docs/architecture.md"
cp "$source_root/tests/fixtures/basic/root/docs/architecture.md" "$project/docs/architecture.md"
shin build >"$work_root/restored.json"

cp "$out/docs/kb/generated/manifest.json" "$work_root/canonical-manifest.json"
"$jq_bin" '.session_test_metadata=true' "$work_root/canonical-manifest.json" >"$work_root/noncanonical-manifest.json"
mv "$work_root/noncanonical-manifest.json" "$out/docs/kb/generated/manifest.json"
reject "$work_root/noncanonical.json" resume --require-current
check_json "$work_root/noncanonical.json" '.snapshot_inputs_current and (.index_current|not) and .continuation_pack==null and .index_freshness.mode=="full-generated-output"'
reject "$work_root/noncanonical-capture.json" capture --require-current
mv "$work_root/canonical-manifest.json" "$out/docs/kb/generated/manifest.json"

cp "$work_root/checkpoint.json" "$project/checkpoint.json"
reject "$work_root/inside.json" shin resume --checkpoint "$project/checkpoint.json" --task-id task-a --thread-id thread-a
check_json "$work_root/inside.json" '.error.code=="session-file-inside-project"'
ln -s "$work_root/checkpoint.json" "$work_root/symlink.json"
reject "$work_root/symlink.json.out" shin resume --checkpoint "$work_root/symlink.json" --task-id task-a --thread-id thread-a
check_json "$work_root/symlink.json.out" '.error.code=="invalid-session-file"'
ln -s "$project" "$work_root/project-link"
reject "$work_root/parent-symlink.json" shin resume --checkpoint "$work_root/project-link/checkpoint.json" --task-id task-a --thread-id thread-a
check_json "$work_root/parent-symlink.json" '.error.code=="session-file-inside-project"'
"$jq_bin" '.state.current_slice="Tampered"' "$work_root/checkpoint.json" >"$work_root/tampered.json"
reject "$work_root/tampered-result.json" shin resume --checkpoint "$work_root/tampered.json" --task-id task-a --thread-id thread-a
check_json "$work_root/tampered-result.json" '.error.code=="checkpoint-integrity-mismatch"'

for mutation in \
  '.unknown=true' \
  '.state.assertions[0].paths=["../outside.md"]' \
  '.state.assertions[0].paths=["docs/architecture.md","docs/architecture.md"]' \
  '.state.assertions[0].receipt_ids=["absent"]' \
  '.state.assertions[0].verification_level="executed"' \
  '.receipts[0].trusted=true' \
  '.receipts[0].ttl_seconds=0' \
  '.receipts[0].observed_at=18446744073709551615' \
  '.receipts[0].command_fingerprint="not-a-hash"' \
  '.ttl_seconds=604801' \
  '.task_request.budget.max_records=25' \
  '.state.next_actions[0].argv=["touch","arbitrary-file"]'; do
  "$jq_bin" "$mutation" "$work_root/draft.json" >"$work_root/invalid.json"
  reject "$work_root/invalid-result.json" shin checkpoint --input "$work_root/invalid.json" --task-id task-a --thread-id thread-a
  check_json "$work_root/invalid-result.json" '(.ok|not) and .error.category=="usage"'
done
printf '{"schema_version":"a","schema_version":"b"}' >"$work_root/duplicate.json"
reject "$work_root/duplicate-result.json" shin checkpoint --input "$work_root/duplicate.json" --task-id task-a --thread-id thread-a
check_json "$work_root/duplicate-result.json" '.error.code=="duplicate-session-field"'
"$jq_bin" '.state.current_slice=("x" * 270000)' "$work_root/draft.json" >"$work_root/oversized.json"
reject "$work_root/oversized-result.json" shin checkpoint --input "$work_root/oversized.json" --task-id task-a --thread-id thread-a
check_json "$work_root/oversized-result.json" '.error.code=="session-input-too-large"'
"$jq_bin" '.state.current_slice=("sk-" + ("Ab9z" * 12))' "$work_root/draft.json" >"$work_root/secret.json"
reject "$work_root/secret-result.json" shin checkpoint --input "$work_root/secret.json" --task-id task-a --thread-id thread-a
check_json "$work_root/secret-result.json" '.error.code=="secret-like-session-content" and ((tostring|contains("Ab9z"))|not)'
"$jq_bin" '.state.current_slice += " gitleaks:allow"' "$work_root/secret.json" >"$work_root/suppressed-secret.json"
reject "$work_root/suppressed-secret-result.json" shin checkpoint --input "$work_root/suppressed-secret.json" --task-id task-a --thread-id thread-a
check_json "$work_root/suppressed-secret-result.json" '.error.code=="secret-like-session-content" and ((tostring|contains("Ab9z"))|not)'
"$jq_bin" '.task_request[("sk-" + ("Ab9z" * 12))]=null' "$work_root/draft.json" >"$work_root/secret-key.json"
reject "$work_root/secret-key-result.json" shin checkpoint --input "$work_root/secret-key.json" --task-id task-a --thread-id thread-a
check_json "$work_root/secret-key-result.json" '.error.code=="secret-like-session-content" and ((tostring|contains("Ab9z"))|not)'
secret_task="$($jq_bin -r '.state.current_slice' "$work_root/secret.json")"
reject "$work_root/secret-task-result.json" resume --task "$secret_task"
check_json "$work_root/secret-task-result.json" '.error.code=="secret-like-session-content" and ((tostring|contains("Ab9z"))|not)'
cat "$work_root/draft.json" | shin checkpoint --input - --task-id task-a --thread-id thread-a --observed-at 1000 >"$work_root/stdin.json"
capture >"$work_root/file-input.json"
cmp "$work_root/file-input.json" "$work_root/stdin.json"

"$jq_bin" '.state.assertions += [{id:"vpn",kind:"blocked",text:"Operator must restore network access.",paths:[],receipt_ids:[],blocker_kind:"vpn"}]' "$work_root/draft.json" >"$work_root/blocked-draft.json"
shin checkpoint --input "$work_root/blocked-draft.json" --task-id task-a --thread-id thread-a --observed-at 1000 >"$work_root/blocked-checkpoint.json"
shin resume --checkpoint "$work_root/blocked-checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1020 >"$work_root/blocked.json"
check_json "$work_root/blocked.json" '(.continuation_ready|not) and any(.next_actions[];.code=="resolve-declared-blockers")'

"$jq_bin" '.state.assertions=[{id:"missing",kind:"completed",text:"Source exists.",paths:["src/does-not-exist.ts"],receipt_ids:[]}]' "$work_root/draft.json" >"$work_root/missing-draft.json"
shin checkpoint --input "$work_root/missing-draft.json" --task-id task-a --thread-id thread-a --observed-at 1000 >"$work_root/missing-checkpoint.json"
shin resume --checkpoint "$work_root/missing-checkpoint.json" --task-id task-a --thread-id thread-a --observed-at 1020 >"$work_root/missing.json"
check_json "$work_root/missing.json" '(.state.assertions[0].references_current|not) and .state.assertions[0].review_required and .review_required'

python3 "$source_root/tests/run_session_lock_tests.py" "$tool" "$project" "$profiles" "$out" "$work_root/draft.json"
echo "Hermetic session checkpoint/resume tests passed."
