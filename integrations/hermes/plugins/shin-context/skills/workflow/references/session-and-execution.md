# Session And Execution

Read only for manual checkpoint/resume, change-aware impact or evidence from an independently authorized host check. These procedures are not part of each retrieval call. Use the exact adapter route and Core contract directory from the main skill.

## Session Recovery

Require Core2.0 and honor a higher consumer minimum. Native MCP is the normal
start route; use the resolved Core CLI through terminal tools only for explicitly requested
`checkpoint --input <external-draft.json> --task-id <task> --thread-id <thread>`
and `resume --checkpoint <external-checkpoint.json> --task-id <task> --thread-id <thread> --require-current`.
Read `session-checkpoint-v1.json` and `execution-receipt-v1.json` from the resolved Core contract directory
for the bounded draft shape. Keep state outside the consumer and
generated index. Require exit zero and `schema_version=shin.session-checkpoint.v1`,
not a universal `ok` field; persist via a private sibling temporary file and
atomic rename, preserving the previous checkpoint on failure. Retain the path plus
binding in host handoff state. Capture manually at logical milestones, after a
long tool operation, before handoff, or before an anticipated interruption.
Checkpoint may capture dirty work; resume reports drift before allowing a fresh
continuation Pack. Inspect `continuation_ready`, `review_required`, and receipt
TTL separately. Saved assertions remain `session-asserted`; producer trust must
come from explicit host policy. This workflow does not propose or install
automatic compaction-triggered persistence, host hooks, or chat storage; those
are outside Shin's host-neutral scope.

With Core2.0, run the explicit read-only `impact` on the same route
before rebuilding a stale index to obtain changed member ranges and declared
dependency neighbors. `coverage.inspection_complete` combines bounded path and
evidence completeness, not complete semantic impact. Inspect per-path omissions, `source_state` and
scope `basis`; historical ranges are not current evidence. `tests_to_review`
does not mean test execution. This terminal workflow never publishes or runs
actions automatically; the Hermes plugin does not reimplement the impact logic.

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
