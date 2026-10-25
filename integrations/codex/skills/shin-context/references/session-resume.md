# Session Recovery

For ordinary context loss or a changed topic, select the exact enrolled project
and ask one useful current-code query. Read known files directly. Optional
`task_key` and `intent` labels describe caller-known work, without inferred host
compaction or per-call model identity. Unknown labels stay omitted.

When the connected server advertises `shin_checkpoint` and `shin_resume`, an
optional useful milestone or handoff may retain compact task notes under the
host's existing permissions. Use the actual host `session_id`, stable caller
`task_key`, exact project and objective. Save a short `progress`, focused
`queries`, optional `paths` and `next_steps`. Keep that binding and the returned
revision in host handoff state; the kit owns private storage outside the project.
Never put chat IDs or notes in shared project instructions or MCP configuration.

After reconnect, resolve the saved exact root through the current catalog when
advertised. A previous connection's selector or snapshot is not a root identity.
After context loss, resume only with the known binding and current objective.
`ok` means assessment succeeded. Review `continuation_ready`, `review_required`
and drift/age findings independently; `ready_without_review` combines readiness
with no pending review. Notes are caller assertions, not source authority,
execution receipts or permission to run saved actions. Replacing notes requires
the exact `expected_revision` returned by resume/save after reviewing its state.

If these tools are unavailable, the binding/key was lost, or no verifiable note
exists, continue with current-code retrieval. Never probe other chat/task IDs,
change identities, or manually rebuild a managed index to recover notes. Let
its service qualify source changes normally. There are no save rituals, call
quotas, automatic pre-compaction capture or host-memory changes.

For the existing lower-level Core CLI procedure, use the same adopted
profile/root/out-dir as retrieval. Its contracts remain `session-checkpoint-v1.json`
and `execution-receipt-v1.json` under the distribution contracts directory
(source layout: `docs/machine`).

Use an external host-owned state directory, for example
`${XDG_STATE_HOME:-$HOME/.local/state}/shin-context/sessions/<project-key>/<task-id>/<thread-id>`.
Derive the project key from the physical repository/worktree identity, not its
basename. Use the actual host task/thread ids and hash or encode them as directory
names. Never copy ids from an unrelated checkpoint to bypass a mismatch. Keep
the checkpoint path and binding in host handoff state.
Avoid the generated index directory, whose contents are disposable.

Create a bounded `shin.session-draft.v1` input. Reuse an explicit reviewed
`shin.task-request.v1` for the continuation evidence requirements:

```json
{
  "schema_version": "shin.session-draft.v1",
  "task_request": {
    "schema_version": "shin.task-request.v1",
    "request_id": "current-task",
    "task": {
      "objective": "Finish the current implementation slice",
      "must": [{"id": "contract", "query": "specific current contract"}]
    },
    "budget": {
      "max_evidence_bytes": 16384,
      "max_records": 12,
      "per_query_records": 6,
      "per_document_records": 2
    }
  },
  "state": {
    "current_slice": "Concrete stopping point",
    "assertions": [
      {
        "id": "pending-check",
        "kind": "pending",
        "text": "Run the focused gate after reviewing the changed source.",
        "paths": [],
        "receipt_ids": []
      }
    ],
    "changed_paths": [],
    "identifiers": [],
    "next_actions": [{"id": "continue", "text": "Concrete next step for review"}]
  },
  "receipts": [],
  "vcs": null,
  "ttl_seconds": 86400
}
```

Capture using the resolved binary and argument vector:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <root> --out-dir <out-dir> checkpoint --input <external-draft.json> --task-id <task-id> --thread-id <thread-id>
```

Require exit zero and stdout's `schema_version=shin.session-checkpoint.v1`;
do not require a universal `ok` field on this data document. Persist through a
private sibling temporary file and atomic rename. Preserve the previous valid checkpoint if capture
fails. `checkpoint` does not write the destination itself. Dirty capture is
allowed; add `--require-current` only when capture must also be a freshness gate.
Do not rebuild just to save task state. Full chat history, credentials, provider
payloads, raw tool output, and large evidence fragments do not belong here.

After an interruption or return to the same task:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <root> --out-dir <out-dir> resume --checkpoint <external-checkpoint.json> --task-id <task-id> --thread-id <thread-id> --require-current
```

Use the returned objective, current slice, assertions, changed verification paths,
continuation Pack, and ordered next actions. `checkpoint_age`, `alignment`,
`index_current`, and `continuation_ready` answer different questions.
`review_required=true` requires reading the reported changes and reconciling state.
Expired checkpoints are hints for review; blocked assertions remain unresolved
until the host/operator rechecks them. A changed `--task` label does not rewrite
the saved request: prepare a new reviewed draft for a changed objective.

`--require-current` rejects stale retrieval while still returning drift and
remediation. Inspect any `next_command.argv` before explicit execution. Run normal
adopted-project readiness gates after publication; resume does not replace them.

Every saved assertion remains `session-asserted`. Unchanged referenced bytes do
not prove the assertion's meaning. A receipt becomes reusable only with explicit
host trust in its producer, matching scope, and a live TTL. Supply
`--trusted-producer <producer-id>` only from established host policy, never because
the checkpoint claims to be trusted. Omit it when provenance is uncertain.
Receipts retain independent results; executed failure is still failure.

Cross-task handoff requires reviewing state and creating a new draft bound to the
receiving task/thread. There is no automatic rebind. Automatic pre-compaction
save, compaction-triggered capture, and envelope injection are outside Shin's
host-neutral contract and are not proposed or installed by this workflow. A host
or operator may invoke the commands manually when recovery requires it.

## Optional request statistics

If the advertised tool schema supports `session_id`, pass the actual host chat
ID when available (Codex: `CODEX_THREAD_ID` from this chat's environment). Keep
that ID across projects in this chat; never invent one from cwd, title or project.
For an explicit query follow-up, pass its `request_trace.request_id` as
`parent_request_id` to read/inspect. These optional fields only correlate local
statistics; omit unavailable IDs and do not restart a working chat to add them.

## Temporary refresh delays

A `recovery.kind=transient` response (updating, busy, refresh timeout or sleep)
only describes this attempt. Continue useful work using direct sources. If Shin
would help at a later task checkpoint, make at most one fresh query for the same
project after `retry_after_ms`; use its new snapshot for inspect/read. Avoid
polling loops, repeated status probes and permanent chat-wide abandonment after
one transient response. Shin remains optional. Report persistent failures with
the exact root and safe reason code; identity, permission, policy and validation
refusals are not transient retry instructions.
