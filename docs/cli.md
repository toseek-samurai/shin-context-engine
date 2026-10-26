# Shin in the terminal

The installed CLI uses the same local service and data home as Monitor. From an
enrolled project directory:

```sh
shin-ce status --text
shin-ce service-status --text
shin-ce query "Where are requests validated?"
```

## Interactive terminal monitor

On macOS and Linux, open the Sand terminal dashboard:

```sh
shin-ce stats
```

The Sand dashboard is included in preview dev.39 and later; Sessions arrived in
dev.45. Controls for Updates below describe unreleased source.
See the [native screen gallery](tui.md), including EN/RU and compact layouts.

For a portable kit, pass `--home /absolute/path/to/private-data`. An installed
kit uses its recorded home. The monitor reconnects automatically when the
service returns, and labels retained observations as stale while disconnected.
Viewing statistics does not start a service or enroll projects. It requires an interactive
terminal with at least 60 columns and 12 rows; text/JSON commands work in pipes.
An 80×24 terminal shows a compact dashboard; wider windows add the observer
column. The header always shows the kit version and observation age.

Overview combines memory, active queries and search p50 with a selectable line
chart and project table. Choose a metric with the mouse or `g`; `h` cycles Live,
one hour and 24 hours. Retained request counts are per minute/hour, while memory
and search time are interval averages. Missing observations stay gaps. Memory
across unverified service-run boundaries is shown as isolated measurements.
Colors, symbols and text distinguish ready, on-demand, preparing and blocked
search; partial source coverage has its own column. Enter opens the cause,
next step and any reported excluded file paths.

| Key | Action |
| --- | --- |
| `1`–`7`, Tab, Left/Right | Overview, Projects, Timing, Storage, Models, Sessions, Updates (Updates is unreleased source) |
| Up/Down or `j`/`k` | Select a project; scroll other views |
| Enter, Esc | Open project details; return |
| PageUp/PageDown | Scroll a page |
| `g`, `h` | Overview chart metric; Live / 1 hour / 24 hours |
| Mouse | Select tabs, metrics, periods and projects; wheel scrolls text |
| `t`, `f` | Toggle recent/retained timings; filter query traffic |
| `p` | Next usage project in Models |
| `r` | Refresh observations; explicitly rescan storage in Storage |
| `u` / `7` | Open Updates (unreleased source) |
| Updates: `r`, `v` | Check public releases; read the saved update result |
| Updates: `i`, then `y` / `n` | Review installation; explicitly confirm / cancel (`Esc` also cancels) |
| Updates: `a` | Acknowledge a verified saved result using its exact revision |
| `?`, `q` | Keyboard help; quit |
| Ctrl-L | Redraw the screen |

The FTXUI renderer uses the same service telemetry and normalized usage data
as Monitor. It requires no curses/terminfo package. Colors adapt to the terminal;
`NO_COLOR` keeps a monochrome view. Basic `vt100`/`ansi` terminals use a simple
character plot instead of Braille.

Unreleased source adds an **Updates** view on macOS. It uses the existing native
updater and the installed kit's recorded home. Opening the view performs no network
request; `r` checks the public GitHub feed. Release notes are shown as a URL. The
review pins the offered revision; neither Enter nor opening the tab installs it.
Only `i`, then `y` starts installation. Checks run off the render thread and can
be abandoned when quitting. Once installation or acknowledgement starts, `q`,
Ctrl-D and Ctrl-C do not cancel it; the screen stays available until the native
operation settles. Forced process termination still relies on the updater's
durable recovery journal.

The same twelve-step plan shows observed completed steps, current stage, elapsed
time, real download/staging bytes and project counts. There is no estimated
overall percentage. Completion comes from the native receipt, independently of
progress. `v` reads retained outcomes; a recoverable result exposes `a` only when
the native status permits acknowledgement. After a successful swap, restart
Monitor, `shin-ce stats` and persistent MCP clients to load the new binaries. Linux shows
the published-kit installation route; automatic release download remains macOS-only.

Service observations refresh every two seconds. Storage is inspected when you
open its tab or press `r`, not continuously. Usage is read every 30 seconds or
on refresh, using the same normalized file and parser as Monitor:

```sh
shin-ce stats --usage-file /absolute/path/to/usage/state.json
```

By default, Models reads the automatically collected `usage/state.json` in the
selected data home. `--usage-file` overrides that path; it must point to a private
collector snapshot, not a configuration file. If no snapshot is available,
Models shows setup guidance. No client-session scanning or usage
collection is performed by the TUI. Partial, stale, and missing measurements
remain explicit. Project management and cleanup use the existing CLI commands;
the first TUI version is for observation.

`status` describes the current project. `service-status` lists all projects in
the selected service, their search and observer states, recent memory usage,
active queries, and request counters for the current run. Omit `--text` for JSON.

Text status distinguishes missing measurements from zero and preparing or
standby readers from blocked projects. A standby project is still observed; its
search reader warms when needed.

Since preview dev.39, Shin keeps verified partial context available when a previously
qualified canonical route exceeds its approved source-file count or per-file byte limit. The
response has `retrieval_ready=true`, `canonical_ready=false` and
`source_coverage.reason=source-observation-limited`, with source counts from the
background observation. Only unchanged files from the approved snapshot are
returned; new, changed, deleted or unverified files are withheld. This does not
publish an incomplete candidate, enlarge a limit or change a profile. When the
inventory fits again, normal background refresh and quality checks resume.
Snapshot corruption, changed authority and unsupported policy remain refusals.
Monitor and the terminal explain this partial state; the copied diagnostic
includes the bounded inventory counts.

A pending read wakes an idle project's background qualification and receives
priority between refreshes. Only one refresh runs at a time; due background work
still receives round-robin turns. Existing source checks, failure backoff and
publication authority remain enforced. CLI reader admission waits at most five
seconds, within the existing 30-second request budget. If preparation is still in
progress, the call returns `project-updating` while background work continues.
`service-status` reports this bound as `admission_wait_limit_ms`.
A reader scheduling hint remains valid for at most 30 seconds after the initial
request, allowing qualification to finish after an unrelated refresh releases the
publisher. It neither extends the caller's wait nor overrides failure backoff.

## Monitor and CLI

| In Monitor | In the CLI today |
| --- | --- |
| Project readiness and partial coverage | `shin-ce status --text` |
| All projects, service health, recent RAM and request counts | `shin-ce service-status --text` |
| Detailed request telemetry and retained history summaries | TUI Timing with recent/history and traffic filters; `shin-ce service-status` JSON |
| Add a project from a folder | `shin-ce project-add --root /absolute/path/to/project` |
| Storage inventory and cleanup plan | TUI Storage or `shin-ce storage-status` JSON; cleanup remains a separate command requiring the reported revision |
| Profile changes requiring review | `shin-ce profile-review`; acceptance is a separate explicit action |
| Model usage | TUI Models reads automatic enrolled-project collection; `--usage-file` remains an explicit override |

The TUI shares Monitor's project-state and request-timing calculations. It uses
native service status for observations and the existing read-only storage
inventory command. JSON remains available for automation.

Monitor and stats support English and Russian. Monitor's language selector uses
System, English or Русский and applies immediately without restarting. Stats follows `LC_ALL`, then
`LC_MESSAGES`, then `LANG`; unsupported languages fall back to English. Override
with `shin-ce stats --language en` or `--language ru`, or press `l` to switch for
the current terminal session. Russian text requires a UTF-8 terminal locale.
This choice affects display text only: command
names, JSON fields, error codes and stored data keep their existing format.

## Inspect evidence

Queries return structured evidence and a snapshot identifier. Use that returned
identifier with project-relative paths for the following reads:

```sh
shin-ce inspect src/example.cpp --snapshot SNAPSHOT_ID
shin-ce read src/example.cpp --snapshot SNAPSHOT_ID
```

Pass `--root /absolute/path/to/project` when working from another directory. A
portable extraction also needs the same explicit `--home` used at initialization.
See [installation](local-kit.md) and [MCP setup](native-client-setup.md).

Shin's usage collector observes supported local client usage records. It is not
required for indexing or retrieval. Adding a project enables automatic collection
for that project. Counters depend on readable client history; missing history is
reported as incomplete. See [the local kit guide](local-kit.md)
for configuration details.

Numerical service history resumes automatically after temporary SQLite contention,
capacity or supported write-I/O errors, with a bounded retry interval. Committed
rows and aggregates stay in the same database. Corruption, unsupported schemas and
changed file ownership or identity remain hard refusals. History is best effort:
a failed batch is never blindly replayed. In service-status JSON,
`dropped_current_run` counts discarded observations; `unconfirmed_current_run`
counts observations whose COMMIT was not acknowledged. Monitor marks either as a
history gap. `retry_pending`, `recovery_attempts` and `recoveries` describe recovery
for the current service run; missing observations are not reconstructed.

### Interpreting service activity

Timing shows separate current-service-run counters for query, inspect, read and
project status calls. Project details expose the same breakdown (Monitor tooltip,
TUI details and service-status JSON). These counters include completed or refused
observed calls; they reset with the service, and a project's counters reset when it
is detached. They are separate from retained request history and query timings.
Successful status checks do not change the historical retrieval count.

Diagnostic calls are explicitly tagged. Other calls remain unclassified: they
may include agent traffic, CLI checks or installation probes. `reported_sessions`
counts distinct valid opaque IDs actually supplied by callers (bounded to 256 for
the run and 64 per project, with `+` displayed on overflow). Calls without such an
ID are shown separately. Session counts do not identify models or measure all chats.
Catalog/help calls handled inside the MCP adapter and service monitoring polls are
excluded from these **service-run** counters; client token counters are a different
measurement. No historical status or missing session information is fabricated.

The dev.45 source also exposes **MCP calls · history** in Monitor Timing,
the terminal Timing view and `service-status` JSON (`telemetry.mcp_activity`). This
separate journal includes status, catalog/help, unknown tools, argument errors and
connection failures, even while the service is unavailable. Initial and terminal
observations count as one invocation. A service request ID links the layers;
their counts must not be added together. Protocol initialization, tool-list discovery
and malformed frames are outside this tool-call count.
Only upgraded MCP adapters contribute; reconnect existing clients after installing
the kit. Older processes and earlier calls are not reconstructed.

The local numerical journal survives adapter/service restarts. It retains up to
seven days and 10,000 calls in a database capped at 32 MiB, under the recorded data
home's `mcp-history` directory. The start of coverage is shown explicitly; earlier
traffic is not reconstructed. Writes run in the background, with bounded queues.
Gaps and unfinished calls remain visible. Successful stdout writes do not establish
client receipt or task success. Queries, source bodies, raw chat IDs and absolute
project paths are not retained. See the [activity contract](machine/mcp-activity-v1.json).

MCP project status returns promptly when a reader needs background preparation.
`project-updating` is a transient observation; a subsequent useful query retains
its normal bounded readiness wait. CLI project status keeps its existing waiting
behavior. Query/inspect/read tools accept `diagnostic: true` for explicit probes.

### Requests during contention (dev.45)

A brief overlap between requests for one project now uses a bounded FIFO admission
queue. Waiting does not occupy another project's execution slot. The existing
limits remain two executing requests, four waiting requests and eight connections.
A ready reader's execution queue waits at most 500 ms; cold preparation keeps its
five-second ceiling. Reader readiness and completed requests wake admission
immediately. Transitioning from cold preparation to an occupied execution slot
also caps that phase at 500 ms. Repeated internal checks never extend a deadline.

MCP project status stays nonblocking when a reader is occupied. The adapter also
keeps status, project discovery and static help available while its two source
operation slots are occupied, within the unchanged eight-request total bound. A persistent
`service-busy` response can include `shin.local-service-admission.v1` metadata
identifying project contention, global execution pressure or a full waiting queue.
This is not a project validation failure. Continue useful direct-source work and,
if still needed after current calls finish, make one useful query for that project.
Neither polling nor manual index repair is required.

A queued request is executed at most once. Disconnecting a waiter releases its
slot. A peer's cancellation does not cancel another waiting request; transient
reader loss preserves its original deadline. Lifecycle, publication authority and
snapshot/source freshness are rechecked before dispatch, so queuing cannot make
an old snapshot usable again.

### Sessions and optional recovery notes (dev.45)

Monitor now has a **Sessions** view; `shin-ce stats` opens the same observations
with key **6**. The latest eight correlated sessions show context calls, errors,
caller task labels, recovery requests and recent invocation outcomes. Calls can
supply `task_key` and `intent` (`explore`, `follow-up`, `context-recovery`); labels
are optional and do not impose quotas or create inferred tasks.

The registered Codex collector also projects explicit host compaction and turn
markers. This metadata is bounded and contains no prompt, summary or source text.
A model in client history is shown separately from a model reported in a call;
it never fills an unknown call model. Missing markers, unsupported clients and
partial history are not evidence that compaction did not happen. Timing describes
retained successful query starts, not actual recovery or context consumption.

The same view now summarizes the latest three observed **episodes** per session:
a client turn start or explicit compaction opens an interval, and the next
explicit boundary closes it. This is timestamp association, not topic detection.
The open interval stops at the last registered Codex collection; calls after it,
before a known start, on a tied boundary, or across conflicting turn identities
remain unassigned. Stale, clipped or incomplete intervals are labelled. Analysis
uses the recorded data home's registered history, independently of an optional
imported token-usage file.

**Read statistics cover MCP `shin_read` only**, excluding explicit diagnostic
probes, failures and writes that did not complete. Query fragments and ordinary
shell, CLI or IDE reads are outside this count. Opaque installation/project-bound
file and version labels distinguish the same version read again from an additional
version; unrelated index-generation changes do not change a file's version.
Within an episode, previously seen versions are separate from repetitions inside
that interval. Existing rows without labels stay unknown. All retained calls are
analysed in the background; the displayed episode limit does not sample their
counts. Repeated reads may be useful and do not measure wasted work or forgetting.

For a useful stopping point, `shin_checkpoint` accepts the selected project,
actual `session_id`, a stable `task_key`, current `task`, concise `progress`, and
one to four focused `queries`. Optional `paths` and `next_steps` preserve a small
explicit handoff. `shin_resume` takes the same binding and current objective.
Notes live outside the project in the recorded data home's private `sessions`
directory. Replacing a saved note requires its returned `expected_revision`.

Resume reuses Core's age, objective and worktree checks. Review its flags and
current evidence before continuing; a note is a caller assertion. Core readiness
and review are independent: the wrapper also reports `ready_without_review`,
which requires readiness and no pending review. These explicit
operations audit the worktree, so reserve them for useful handoffs. Ordinary
context queries keep their existing fast path. Missing or refused checkpoints
are optional: continue with a useful current-code query and preserve saved state.
See the [recovery contract](machine/local-session-recovery-v1.json).

### Session and model attribution (dev.14)

For unfamiliar code in enrolled projects, agents use Shin first; known files and
symbols can be read directly. If retrieval fails or is insufficient, continue
with ordinary sources. There are no call quotas or per-edit rituals.

MCP project tools accept the actual host `session_id` and, when explicitly known,
`model_id` for that call. A model ID requires a session ID. The adapter hashes the
session before local RPC; raw chat IDs are not stored in telemetry. It never derives
a chat or model from shared process state. Clients must omit unavailable metadata
and refresh model identity after a model switch. The CLI offers `--session-id`,
`--model-id`, and `--parent-request-id` (read/inspect only). Use only flags/schema
fields advertised by the installed version.

Model labels are **caller-reported, not verified**. Current-service activity shows
up to 16 labels and separate unknown/overflow call counts. Historical request rows
retain the model with the opaque session; `history.model_cohorts` summarizes up to
16 labels, with a truncation flag. Empty model IDs mean unknown, including legacy
rows. This additive history column preserves earlier rows and allows older readers
to ignore it. Status calls remain current-run only. Token-usage records are separate;
no join by timestamp, project or default model is used to claim attribution.

If sessions are present but every call has an unknown model, check the actual
tool arguments: a client can supply `session_id` while omitting `model_id`.
This is missing per-call metadata, not a failure to retain the supplied label.
The native usage collector can separately read model names from client history;
those names do not identify the model behind a particular MCP call. Supplying a
default model in shared client configuration would also mislabel model switches.

### Release update checks (macOS)

`shin-ce update-check [--prefix DIR]` reads the public releases feed and returns
an eligible newer kit with an opaque `revision`. It performs no installation.
`shin-ce update-install --revision HASH [--prefix DIR]` revalidates that offer,
verifies the download and calls the new kit's native updater. A changed offer
requires a fresh review. `shin-ce update-status [--prefix DIR]` reads the saved
outcome without networking. `update-download --revision HASH` verifies and stages
a kit without applying it. These commands use the recorded data home, preserve
client configuration and never change project authority. See the
[local kit guide](local-kit.md) for Monitor checks and recovery boundaries.

`update-acknowledge --revision REVIEW_HASH` explicitly acknowledges a saved result
only after the installed payload is verified and its native recovery journal is
complete. Monitor exposes this through **Last update result…**. Stale review
hashes or a pending recovery remain refusals; acknowledgement never repairs an
index or replays an installation.
