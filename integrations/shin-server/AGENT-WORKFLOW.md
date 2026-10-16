# Headless Shin Agent Workflow

Development-only entrypoint for native Core, Server and Observer. The installed
Stable tool and global skills remain independently maintained. Use this file and the adjacent
[request examples](workflow-examples.json); do not infer a client from test code.

## Reviewed Binding

Before starting, obtain the operator's explicit binding: absolute Core/Observer
binaries, source root, profile id/registry, separate local index and private
state directories, loopback service URL, independently pinned Server identity,
principal id, project/workspace ids and exact route or publication selector.
Use real host task/thread ids (synthetic ids only in an explicit fixture).
Never choose an output by recency, borrow another task's ids, or learn the
expected Server identity from an unauthenticated response.

The operator prepares a private mode 0600 curl configuration outside source:
Authorization bearer header, X-Shin-Server-ID, JSON Content-Type, connect
timeout and total timeout. Do not print/read its credential contents into
model context, put tokens in argv, enable curl tracing, follow redirects, or
change the destination. This guide uses these reviewed shell variables:

- `CORE`, `OBSERVER`, `ROOT`, `PROFILE`, `REGISTRY`, `LOCAL_OUT`, `STATE`
- `CURL_CONFIG`, `SERVER_URL`, `PROJECT`, `WORKSPACE`, `ROUTE`, `SLOT`
- `TASK_ID`, `THREAD_ID` and `OBSERVER_CONFIG` (private source config v2)

The local index, state, reports, requests and Observer state must be disjoint
from source and from one another as required by their contracts. Set `umask 077`.
Check binary versions; Server and Observer must name the intended Core version.
No automatic installation, configuration upgrade, route registration or migration.

## HTTP Discipline

Use curl with `--disable` first to ignore ambient curlrc, an explicit private
config, no proxy for loopback, no redirects, and capture status separately:

```sh
curl --disable --config "$CURL_CONFIG" --noproxy '*' --silent --show-error \
  --request POST --data-binary "@$STATE/request.json" \
  --output "$STATE/response.json" --write-out '%{http_code}\n' \
  "$SERVER_URL/api/v1/session-bootstrap"
```

Inspect both HTTP status and the JSON envelope. Transport failure has no
successful Server result. A 200 assessment is not necessarily ready/current.
Never run response-provided commands without reviewing them.
For registration, preparation and publication proposals, retain an `Idempotency-Key` header.
Checkpoint mutations instead carry their own exact request body/id/deadline.

## Attach And Obtain Context

1. With explicit operator approval and administrative permission, POST the
   `project` example to `/api/v1/projects`, then `workspace` to
   `/api/v1/projects/{project}/workspaces`. Register the exact reviewed root.
   Existing attachments can be read instead. Registration does not index or
   grant access; local routes/profile paths must already be operator-configured.
2. For an explicitly approved local route, GET its `/activation`, retain its
   revision/snapshot, then POST `prepare` to
   `/api/v1/projects/{project}/routes/{route}/preparations`.
   `expected_snapshot_id` is null only for an empty head, otherwise the observed
   snapshot. Poll the returned job id at `/api/v1/preparations/{id}` until a
   terminal state; `prepared` alone is not publication authority.
3. Read `/api/v1/preparations/{id}/inspection` and `/preflight`; require passing
   applicable quality and explicit approval before POSTing `activate` to the
   route's `/activation` with the observed revision. Conflicts require rereading,
   not blind retries with a new revision. Shared-publication selectors use their
   separate proposal/review flow, never automatic route activation or fallback.
4. With reviewed knowledge maintenance configured, an authorized operator may
   POST `refresh` to `/api/v1/knowledge-maintenance/refresh`. This requests work,
   not immediate health. Bootstrap itself never schedules scans or repairs.
5. POST `bootstrap` to `/api/v1/session-bootstrap` using the exact selector.
   Inspect `ready`, `context_ready`, `health_ready`, member errors/coverage and
   admitted provenance separately. `checkpoint_restored=false` is expected.
   Missing/expired health is not permission to claim ready. Verify returned
   relative source paths in current source.

For shared-publication submission on Server dev33/schema019, read its head and
use the observed `history_epoch` alongside `preparation_id` and `base_revision`.
Omission means epoch0, not automatic selection of the current epoch. Keep an
old request body/key unchanged for replay. `publication-history-changed`
requires explicit review before authoring a new request, never a blind retry
with the new epoch. Shared revision and admission epoch are separate counters.

## Publication History Maintenance

Only an explicitly authorized administrator may retire global history. Inspect
`GET /api/v1/publication-history` and its `/retention?limit=64` plan. Audit-pinned
records require the separate reviewed audit export/retention workflow first.
Apply the exact plan with `POST /api/v1/publication-history/retention` and
`{"plan": <inspected-plan>, "discard_confirmed": true}`. Retain the plan/receipt
outside consumer source; retry the same latest plan after a lost reply. Scope
is all projects and new admission uses the new epoch. Do not clear queues,
raise quotas or delete history automatically when ordinary work hits capacity.
The matching `shin-publication-history-v1.json` contract owns protections,
limits, explicit discard and rollback; an empty plan is not permission to force
deletion of protected state.

## Revoked Observer History

Default to retaining unconfirmed history. An authorized operator may first use
Observer `outbox-export` and `outbox-verify` for separate private local custody;
that archive never confirms Server history. Do not revoke or discard an offline
stream solely because time elapsed.

On Server dev34, an administrator explicitly authorized to accept data loss may
inspect `GET /api/v1/observer-streams/{stream}/discard`. Review its revoked
identity, counters, `unconfirmed_submissions`, deleted totals and content hash.
Only an eligible plan may be POSTed to the same route with
`{"plan": <inspected-plan>, "confirmation": "discard-unconfirmed-history"}`.
This deletes retained Server history, including the unconfirmed suffix; it does
not prove client acknowledgement or archive durability. Retain the exact
plan/receipt outside source for retry after a lost response. A stale plan needs
new review, not blind replacement.

The retired ID remains permanently blocked. Continue only through an explicitly
reviewed new stream/state namespace with zero inherited acknowledgements.
Tombstone capacity still fails closed; never delete protected identities or
reset a queue to bypass it. Explicit offline namespace retirement is described
below. Existing `/reclamation` remains the fully confirmed
path. The `unconfirmedDiscard` section in `shin-observer-registry-v1.json` owns
the detailed contract; local files, publication state and source are untouched.

## Retired Identity Capacity

Server dev35 offers an explicit downtime procedure, not online automatic GC.
Stop/fence all Server instances. With private schema-owner recovery config, run
`observer-retirement-plan --config FILE --input REQUEST`. The request has
`schema_version: shin.observer-retirement-request.v1`, a fresh random32-hex
`operation_id`, the inspected `expected_server_id`,
`reason_code: retired-identity-capacity` and `service_fenced: true`.

Review the complete plan and its counts: all active streams will be revoked,
all retired-ID metadata discarded, and the deployment pin changed for every
client. Stream rows, unconfirmed history, receipts, audit and publications remain.
Only explicit approval of that scope permits `observer-retirement-apply` with
`{"plan": <inspected-plan>, "discard_confirmed": true}`. Preserve the exact
request/plan/config and receipt for uncertain-response retry. Busy is a refusal,
not success; retry still requires the offline fence. Never replace a stale plan
without review.

After commit, explicitly update the service pin to the receipt's successor and
review every client binding. Retained local queues cannot be repinned or reset;
use the existing archive/disposition and fresh-state enrollment procedure.
If the retained full-stream registry is also full, explicitly reclaim confirmed
revoked rows or approve their separate unconfirmed discard before enrollment;
retiring tombstones alone does not release live-stream slots.
Identity is scoped by Server ID plus stream ID. The old complete HTTP request
stays rejected even if a bare ID is later deliberately registered under a new
pin; no global uniqueness or continuity of that bare ID is implied. Ordinary
`recovery-plan/apply` remains non-destructive to tombstones. The registry
contract owns these limits; this procedure does not certify external archives.

## Preserve A Task And External Check

Keep a reviewed `draft` from the examples in private host state. Assertions are
session claims, not project authority. Never store chat transcripts or secrets.

Maintain an explicit local publication for offline use. Only an authorized
local build publishes it; Server-managed artifacts are a different private
publication and are not the local CLI out-dir. Capture a local checkpoint too:

```sh
"$CORE" --profile "$PROFILE" --profile-dir "$REGISTRY" --root "$ROOT" \
  --out-dir "$LOCAL_OUT" checkpoint --input "$STATE/draft.json" \
  --task-id "$TASK_ID" --thread-id "$THREAD_ID" --require-current
```

Persist successful JSON stdout through a private temporary file and atomic
rename. Never overwrite the previous checkpoint on failure. Without a prior
local checkpoint/draft, an outage cannot magically restore Server-only state.
Keep critical commands separate: inspect the checkpoint process result and
expected schema before issuing a standalone rename between the two reviewed
state paths. Do not append unrelated commands to that rename. Do not substitute
a hard link: a leftover temporary alias could later overwrite the checkpoint.
If client approvals still refuse the rename, leave the temporary file pending
and report the blocked step; never relax client approvals to finish the task.
For local integrity use `check` without `--require-current`; use current Brief
or resume where that option is supported to assess source freshness.
CLI outputs are not uniformly HTTP-style envelopes: `receipt-scope`,
`receipt-import` and `checkpoint` return typed data documents and may have no
`ok` field. Check process success and the expected `schema_version`; do not
reject a valid document merely because `.ok` is absent. For HTTP checkpoint
metadata, retain the envelope's `.result`, not the boolean result of a jq test.

For a separately authorized external check, capture `executionInputs` using
`receipt-scope`, then execute the check outside Shin. Preserve its private JUnit
or Playwright report. Fill `reportImport` with the true captured/start/end times,
principal id, command fingerprint and explicit completeness; invoke:

```sh
"$CORE" --profile "$PROFILE" --profile-dir "$REGISTRY" --root "$ROOT" \
  --out-dir "$LOCAL_OUT" receipt-scope --input "$STATE/inputs-request.json" \
  --task-id "$TASK_ID" --thread-id "$THREAD_ID" --require-current
# Persist this successful result as inputs.json before executing the check.
# Execute the independently authorized check and retain its actual report.
"$CORE" --profile "$PROFILE" --profile-dir "$REGISTRY" --root "$ROOT" \
  --out-dir "$LOCAL_OUT" receipt-import --input "$STATE/report-import.json" \
  --task-id "$TASK_ID" --thread-id "$THREAD_ID" --require-current
```

Core import normalizes a report; it does not authenticate its producer.
The host owns the command fingerprint convention. A recommended descriptor is
a compact UTF-8 JSON array of sanitized argv, with no trailing newline, hashed
with SHA-256. Do not join arguments with spaces: that loses argument boundaries.
Do not put credentials in this descriptor or assume its hash attests execution.
Put the returned receipts in the reviewed draft. GET the private checkpoint
metadata to obtain revision (zero only for a confirmed `checkpoint-not-found`), then POST `save` to
`/api/v1/projects/{project}/task-checkpoints/{slot}` with that draft. Use a new
random 32-hex request_id and a deadline strictly after now and at most 300s ahead.
Retain the complete body for retries. Save returns metadata, not checkpoint text.

Only when the operator has granted `receipt_producer`, POST `confirmReceipts`
to the slot's `/receipts`: exact saved receipt objects, authenticated producer id,
task/thread and the newly returned revision. Confirmation advances revision.
An admin name or permission alone does not trust embedded receipts.
Ordinary save clears all prior confirmations, even when receipt bodies match.
Expired, failed, incomplete or changed-input receipts cannot become passed-current.
Authentication is presenter provenance, not independent execution attestation.

On a lost mutation response, retry the identical body only within its deadline.
After expiry inspect metadata and reconcile before forming another request.
Do not recompute the old request at a new clock time and call it an identical retry.

## Return, Change And Review

A returning agent loads only this entrypoint, the reviewed binding and its
private persisted task state. Read checkpoint metadata and POST `resume` to
the slot's `/resume` with the same task/thread. Inspect checkpoint revision,
source provenance, `assessment.ok`, `checkpoint_age`, `index_current`,
`continuation_ready`, `review_required` and each receipt independently.

After an authorized source edit, POST `impact` to `/impact`. An explicit
`require_current=false` permits drift inspection, not current-context approval.
Read named changed paths/members and check scope/TTL. Resume with true must not
approve continuation from stale evidence. Do not suppress this by clearing the
checkpoint, choosing another route, reusing producer names or claiming a rerun.

Only after explicit review may the operator rebuild/publish and the host rerun
checks, form a new draft and save/reconfirm new receipts. A completed task can
be cleared with `clear` and the current revision, then explicitly pruned after
its replay fence expires. No automatic TTL deletion or cleanup on reads.

## Server Outage

Report remote persistence/context as unavailable. Do not acknowledge an offline
HTTP mutation, switch Server identity or upload local state automatically later.

Against the already reviewed local publication and external local checkpoint:

```sh
"$CORE" --profile "$PROFILE" --profile-dir "$REGISTRY" --root "$ROOT" \
  --out-dir "$LOCAL_OUT" resume --checkpoint "$STATE/local-checkpoint.json" \
  --task-id "$TASK_ID" --thread-id "$THREAD_ID" --require-current
"$OBSERVER" sample --config "$OBSERVER_CONFIG"
"$OBSERVER" status --config "$OBSERVER_CONFIG"
```

Core/Observer remain local with Server and PostgreSQL stopped. Stale or expired
state still requires review. Only an explicitly authorized local build and a
fresh native checkpoint can establish a new current local state.
Server receipt confirmation does not silently grant CLI trust; pass
`--trusted-producer` only from a separate established local host policy.

## Observer Recovery

After reviewed Server recovery or destination retirement, keep the old Observer
config, state and matching binaries. Stop its watcher/sender. For Observer dev20
or newer, choose a new archive file in a private0700 directory outside source,
index, profiles and Observer state:

```sh
"$OBSERVER" outbox-export --config "$OBSERVER_CONFIG" --archive "$ARCHIVE"
"$OBSERVER" outbox-verify --config "$OBSERVER_CONFIG" --archive "$ARCHIVE"
```

Review the archive hash, original destination/stream, pending entries and
rejected observations. These commands use no network and do not alter state.
Verification requires the current committed state under its existing writer
fence; a changed state requires a new reviewed export. Preserve an incomplete
or differing archive and choose another file, never force overwrite. The file
contains private paths and metadata, not source/index bytes or credentials.
It is not independently verified backup custody or evidence of delivery.

Explicit operator disposition for this path is **retain old history and begin
new continuity**. Preserve the old files and retire the old invocation from use.
Create a distinct private state directory and a separate config with that
`state_dir`, retaining the reviewed source/profile/workspace binding. Do not
copy old state into the new namespace. Explicit local `sample` creates a new
stream with no destination or prior acknowledgements. With admin approval,
enroll it through `POST /api/v1/observer-streams`: use its new packet's stream_id
and payload identities plus the authorized principal_id. Existing conflicting
old streams require explicit revocation, never identity reuse. Use a separately
reviewed delivery config for the intended Server pin before explicit delivery.

An archive does not authorize replaying old packets to the new destination or
deleting local/Server history. Registry capacity can still block enrollment;
revoked unconfirmed Server history requires its separate R2 disposition work.
See `localArchive` in `shin-observer-delivery-v1.json` for exact fields and bounds.

## Contract Owners

Private checkpoint, authenticated receipt, bootstrap, local Observer and current
configuration contracts remain authoritative in the matching distribution's
`docs/machine`. Historical milestone versions are not current configuration
instructions. Tests replay these examples; a scripted fresh process is not by
itself proof that a new model-driven agent followed the entrypoint successfully.
