# Session And Execution

Read only for manual checkpoint/resume, change-aware impact or evidence from an independently authorized host check. These procedures are not part of each retrieval call. Use the exact adapter route and Core contract directory from the main skill.

## Session Recovery

Start context recovery with a focused native query for the exact project. The
server's advertised tool catalog determines whether optional `shin_checkpoint`
and `shin_resume` are available; a historical Python plugin is not a replacement
for this native capability. Existing host permissions still govern note writes.

For a useful explicit handoff, checkpoint compact progress, focused queries and
optional paths/next steps. Supply the real host `session_id`, a stable caller
`task_key` and the objective. Retain the project/session/task binding, objective
and returned revision in Hermes handoff state. Shin keeps notes in its private
external data home; conversation memory and compression remain Hermes-owned.
Shared project instructions and MCP configuration must not store chat identity
or private notes. No new `.hermes.md`, plugin tool or compaction hook is needed.

After reconnect, rediscover the exact saved root in the current catalog if
available; do not pick by name or treat an old selector/snapshot as identity.
When resuming that binding, `ok` confirms an assessment, not clearance to follow
the saved plan. Inspect `continuation_ready` and `review_required` independently;
`ready_without_review` requires readiness and no outstanding review. Reconcile
changed code, expiry and the current objective. Caller assertions never establish
source authority, receipt trust or authorization to execute their next steps.
A replacement save supplies the reviewed state's exact `expected_revision`.

Missing capabilities, an unknown binding or a refused note do not prevent
ordinary current-code retrieval. Do not borrow another session/task identity,
change project ownership or rebuild a service-managed index. Use the service's
normal background qualification. Optional `task_key` and `intent` labels are
caller reports, never inferred compaction facts or model identity. Save only
when useful; neither turns nor compression impose a call/save quota.

### Existing Core CLI procedure

For a separately authorized Core checkpoint task on an existing explicit route,
require Core2.0 and any higher consumer minimum. Use the resolved CLI for
`checkpoint --input <external-draft.json> --task-id <task> --thread-id <thread>`
and `resume --checkpoint <external-checkpoint.json> --task-id <task> --thread-id <thread> --require-current`.
This does not bypass native refusals. Read `session-checkpoint-v1.json` and
`execution-receipt-v1.json` from the resolved Core contract directory. Keep state
outside the consumer and generated index. Require exit zero and the expected
schema, not a universal `ok`; persist by private temporary file and atomic rename,
preserving the previous checkpoint on failure. Retain the path/binding in host
handoff state. Producer trust comes only from explicit host policy.

An authorized read-only `impact` on that route reports changed member ranges
and declared dependency neighbors. `coverage.inspection_complete` combines
bounded path and evidence completeness, not complete semantic impact. Inspect
omissions, `source_state` and scope `basis`; historical ranges are not current
evidence and `tests_to_review` does not mean test execution. Publication stays
with the route's owner; this procedure never authorizes a managed-index rebuild.

### Optional Execution Evidence

When a host has explicitly authorized an external test or check, follow the
manual workflow in `execution-evidence-v1.json` from the resolved Core contract
directory:

1. `receipt-scope --input <request> --task-id <task> --thread-id <thread>` captures
   `shin.execution-inputs.v1` outside the consumer. Require exit zero and the
   expected schema, not a universal `ok` field, before atomic persistence.
2. The host independently runs the authorized check and its producer emits
   complete sanitized metadata. Shin never runs the command; source isolation
   remains the producer's responsibility. Fingerprint a compact UTF-8 JSON
   array of sanitized argv with SHA-256 (no trailing newline), not space-joined
   arguments. Remove secrets first; a fingerprint is not execution attestation.
3. `receipt-import --input <request> ...` reads one local JUnit XML or Playwright
   JSON report, and `receipt-check --input <bundle> ...` assesses it. Use
   `--trusted-producer <id>` only from explicit host policy. `--strict` requires
   every receipt to pass; `--require-current` separately checks KB freshness.
   Persist the imported `shin.receipt-bundle.v1` only after exit zero and schema
   validation; preserve the previous artifact on failure.

Only normalized counters and bounded metadata are retained. Names, logs,
screenshots and report payloads are not retained. Explicit `declared-complete`
file/tree selectors may narrow applicability; profile fallback is unchanged.
Resume and Impact may embed the same receipts, while receipt failures remain
separate from static inspection completeness. This does not add automatic
execution, compaction hooks, daemons or MCP endpoints.

## Optional request statistics

If the advertised tool schema supports `session_id`, pass the actual host chat
ID when available (Codex: `CODEX_THREAD_ID` from this chat's environment). Keep
that ID across projects in this chat; never invent one from cwd, title or project.
For an explicit query follow-up, pass its `request_trace.request_id` as
`parent_request_id` to read/inspect. These optional fields only correlate local
statistics; omit unavailable IDs and do not restart a working chat to add them.
