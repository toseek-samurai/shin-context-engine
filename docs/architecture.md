# Architecture and scope

Shin is split into components with distinct responsibilities. The current source identity and release boundaries are recorded in [VERSION](../VERSION) and the [machine-readable product manifest](machine/shin-context-engine.manifest.json).

Domain terms are defined in the [glossary](../GLOSSARY.md); platform downloads and
current source have [separate identities](README.md#current-preview).

## Components

**Core** owns profiles, indexing, retrieval, evidence formatting, and the command-line interface. A profile selects the paths and file families that belong in a project context. Use `--out-dir` to keep generated output apart from source; the low-level CLI defaults to the project root when this option is omitted. Core reads do not grant authority to publish or execute anything.

**Observer** owns local project observation, bounded local state, and persisted observation formats. Optional native tools include the `shin-ce` project CLI and terminal monitor, the stdio `shin-mcp` adapter, usage collection, and the desktop Monitor. Observer can use Core source discovery for local source queries. The optional macOS local kit is described in its packaged `local-kit.md` and `native-client-setup.md`.

**Shared code** contains versioned protocols and operating-system support. It does not grant project, publication, or execution authority by itself.

## Authority and state boundaries

Project profiles determine the source scope. Generated indexes and caches are outputs and must be placed separately from source. Observer connections are explicit and project-bound; a read command does not enroll a project or grant publication rights. Existing indexes require an explicit owner decision before management or replacement.

Extracting a kit does not configure client credentials, edit client settings, or register an operating-system service. The macOS installation and service commands are explicit per-user operations. The Windows MSI design uses an elevated machine host to supervise separate unelevated user runtimes; its installation and lifecycle qualification are separate from source testing. Project authority remains with the user runtime, not with the UI or installer.

## Platform and release scope

Core and Observer provide CMake source routes for macOS, Linux and native Windows x64. Windows adds adapters to the same engine, profile registry, freshness checks and versioned protocols. Private storage uses Windows file identities and ACL checks, local transport authenticates named-pipe peers, and worker lifetime is bounded through Job Objects. These adapters preserve the shared authority and refusal contracts.

Published previews provide a macOS Apple Silicon kit and Windows x64 ZIP/MSI
packages. The project-owned Homebrew tap builds the complete macOS application
from source; Linux/WSL2 provides an Ubuntu 24.04 x86_64 terminal archive and source
builds without a native desktop host. The unified CI qualifies each platform
independently: its own Core and Observer jobs and exact-package checks must pass
before that platform's assets are published. Hosted package tests remain
separate from interactive desktop and installed SCM lifecycle checks. See the
[platform capability matrix](README.md#capabilities-by-platform),
[Windows scope](windows-desktop.md) and [build guide](building.md).

Component identities and qualification state come from the checked-in version files and product manifest. Source builds do not qualify a Stable release.

## Current capabilities and agent integration

The architecture exposes search, bounded Brief and task-oriented Pack retrieval,
snapshot-pinned batches, source inspection and reading, and change-aware evidence.
These capabilities share Core's provenance, freshness and output-budget contracts.
Brief, Pack, Eval and Diagnose use the same retrieval decision to report how
the query matched. The decision identifies the search mode and matching policy
independently of the result's presentation.
Agent integration uses native MCP or an explicit CLI adapter. The agent still owns
task execution and conversation memory; a retrieved instruction cannot grant it
publication or filesystem authority.

Consumer profiles describe source selection and product scenarios. Shin owns
algorithm/resource regressions; consumers should
not split or rewrite sound documentation to optimize internal counters. Optional
`mustEvidence`/`mustNotEvidence` checks inspect the selected display fragments in
both Eval and Pack-eval. Paths and lexical coverage alone do not prove an answer.

A request-owned retrieval plan accounts for repeated and rejected attempts and
bounds optional work. Declared document replacements/amendments qualify individual
fragments and add related current owners under existing output and safety budgets.
Cycles, drafts and incomplete relation inventories remain explicit uncertainty.
The [lexical contract](machine/lexical-retrieval-v1.json) owns these rules.

The MCP adapter can pin a follow-up snapshot for one real session and project after
a successful query. An explicit snapshot remains strict; overlapping queries,
unknown identities and a new connection require a snapshot or fresh query.
`context_result` distinguishes delivered evidence from availability, with measured
latency and process-scoped memory. These features are included in the documented
dev.120 packages; a result summary still does not establish answer correctness.

Dev.120 includes styles and web templates in the applicable bundled
bootstrap profiles. The existing native source index searches CSS/SCSS,
HTML/HTM and Razor/CSHTML as bounded text with exact source locators. Source
scope is returned independently of readiness; excluded formats are not implied
to have been searched. No CSS cascade, template rendering or framework symbol
resolution is claimed. Consumer-owned profiles and limits remain authoritative.

## Quality gates and performance limitations

Observer dev.75 adds an idle refresh path shared by Windows, macOS and Linux.
Package availability follows guarded CI publication. The local service retains
one owned serial refresh process rather than starting one for every timer tick.
Native changes still wake the controller; its periodic reconciliation remains
bounded by the existing 15-second idle wait.

After full qualification, a process-local checkpoint can avoid full corpus reads
and content rehashing while native source/output observation is healthy. Each
idle pass still checks complete input membership, file metadata, generated layout,
profile, external dependencies and current project authority. Any change, lost
events, invalid authority or process restart restores the full qualification
path. Checkpoints are never persisted or imported from RPC; service LRU retention is
limited to 16 scopes and 8192 input records in total. Eviction changes cost, not
the profile's coverage or quality bounds.
Small policy and authority files are still read; the diagnostic verification
flags describe the work performed, not raw operating-system I/O counters.

Observer shares a macOS FSEvents stream only between monitors with
identical normalized root sets. Each subscription retains its own predicate,
invalidation token, sticky health and teardown fence. A weak process-local registry
does not retain unused streams; native root/loss events prevent stream reuse.
The existing idle reconciliation, descriptor reserve, cache limits and selected
source verification remain mandatory. Linux and Windows retain their native
backends. Shared-root lifetime, concurrent subscription, root replacement and
controlled descriptor-pressure fixtures cover this optimization.

The service supplies child RSS through a deferred telemetry reader. Its existing
one-second sampling gate runs before querying child processes. Independent
resident-memory retention checks keep their original cadence and limits;
unavailable child RSS remains unknown rather than becoming zero.

Observer runs the project coordinator on statically linked libuv
1.53.0. Native source notifications wake its async handle; borrowed nonblocking
descriptors (POSIX) or one-shot waits on overlapped-operation events (Windows)
wake it for RPC and worker IO. Readiness never accepts, reads or qualifies data:
the ordinary service pump retains framing, peer identity, FIFO admission,
execution limits, cancellation and source verification. Registrations retire
before borrowed channels can close or reuse an OS identity.

The next coalescing, retry, admission, framing, reader or refresh deadline drives
a one-shot timer. Registry reconciliation, telemetry and memory checks retain
their one-second cadence. Lifecycle checks still precede request dispatch and
qualification; idle lifecycle maintenance uses the one-second control heartbeat.
Full idle source reconciliation remains bounded by 15 seconds. CLI stop signals
and the SCM stop event wake the loop.
Native callers supplying only an opaque cancellation predicate retain the former
20 ms probe bound. Storage maintenance waits on its existing five-minute interval
with stop-token cancellation instead of waking every 100 ms. None of these
scheduling hints grants publication authority or changes qualification budgets.

Persistent refresh admission also retains one process-local executable hash behind
healthy native events, fresh file identity, size, modification/change time and
running-image checks. Replacing the named executable invalidates it and the
qualification key. Unavailable events restore full binary hashing. One-shot CLI
admission and final qualification recording read the full executable and check
its identity and metadata before/after, without starting a native event stream.
If a delayed or coalesced directory event arrives during a cached-image read,
Observer discards the cached proof and performs one additional full read against
the original file identity, timestamps and running image. Both hashes must agree.
Actual edits or replacements still refuse admission; directory invalidation alone
does not establish that the executable changed.

An idle observation reports `content_reverified: false` and
`verification_level: source-inventory-and-metadata-observed`; it does not create
a new full-audit receipt. Query/read still verify the selected source bytes.
Current source bounds each active refresh to three minutes, with a 60-second
deadline per stage. A recognized stage change renews the stage budget; repeated
heartbeats, invalid stages and regressing elapsed times do not. Suspend and
cancellation checks remain active. System sleep between completed operations
starts a new operation clock. Bootstrap validation shares one independently
generated expectation across its integrity gates, then transfers the validated
candidate's admission and reader cache to the published generation.

Quality gates combine source/profile validation, generated-index integrity,
declared retrieval cases, Pack completion and resource checks. CTest separately
exercises runtime correctness, isolation, refusal and recovery. A passing
retrieval case is evidence about that declared case, not universal answer quality.

Core can qualify a short query's combined document candidates
before scoring narrowed rows. A successful combined plan avoids repeating that
row pass; rejected plans retain the ordinary fallback and cumulative work budget.
Resource ceilings still count candidate admission and positive scored matches.
Those counts are diagnostic work signals, not query latency or RSS measurements;
reducing repeated work does not necessarily clear a per-pass resource warning.

Performance depends on source volume, document structure, cold/warm reader state,
refresh work and concurrent activity. Measure memory and disk alongside latency.
Neither a fast workstation nor a synthetic workload establishes weak-machine
capacity or token savings. Future work on packaging, platform qualification and
developer workflow must retain these evidence boundaries and the existing limits.

## Source layout and UI ownership

Core lives in `src/` with its tests in `tests/`. Observer owns its
`src/`, `include/`, tests and CMake entry point. `shared/` contains protocol and
platform support; `third_party/` holds vendored dependencies. `profiles/` selects
source scope, and `docs/machine/` holds versioned contracts.

Observer's local status module determines project readiness for CLI, Monitor and
TUI. `ui/` owns the shared timing and usage presentation and the compiled EN/RU
catalog. `tray/` adapts these results to macOS and Windows and `tui/` adapts them
to the terminal. Both desktop hosts use the same native review and apply
operation for selected agent settings. Language preferences never change
protocol fields or project data.

One-shot and resident queries share the context delivery module. It owns source
view projection, response fields and delivery summaries. The bounded renderer
returns its structured receipt alongside the text; internal summaries consume
that receipt directly. Callers retain admission, publication locks and final
source/authority verification. Retained partial context keeps its distinct
verification and unknown-delivery semantics.

Generation completion shares source-cache preparation and the final current
snapshot check. A returned publication qualification includes the completed
cache transfer, so completion does not rebuild that cache. Canonical and
bootstrap routes retain their own authority fences, binding saves and
qualification-recording order; granted route version pins remain immutable.

Background refresh keeps an optional in-process qualification checkpoint. On
macOS its FSEvents stream coalesces source, output and private control roots to
their common ancestor, filtering events back to the selected scope. This reduces
WatchRoot ancestor descriptors; source inventory/metadata, root identities,
publication and authority checks still fence reuse. Resource pressure discards
only the optional checkpoint and restores the full qualification path.
Refresh progress records qualification cache, snapshot, generation, profile,
doctor, adoption, fingerprint and full-readiness stages. These fixed identifiers
and their durations survive a worker timeout in bounded service/history
diagnostics. Each stage retains a 60-second stall limit, with a separate
180-second operation limit; repeated same-stage heartbeats extend neither.
The macOS/Linux Sand TUI uses statically linked FTXUI for layout, input and line charts.
It reads the existing local service, usage snapshot and storage inventory; opening
it does not enroll projects, start another service or publish an index.

The UI source profile also covers the macOS Objective-C++ host and Cocoa method
implementations in its headers. Core observes their selectors and source ranges
without loading Cocoa; include files with a `.inc` suffix still require direct reads.
The former optional web query panel and its HTTP adapter have been removed;
its numeric history, metrics and portfolio producers remain independent tools.

## Persisted compatibility

The standalone Server, HTTP delivery client and deployment tooling have been
removed. Observer sampling, metrics and private outbox export are local-only.
Strict persisted-state validation retains existing observation metadata, identity
pins and stored receipts without sending, acknowledging or deleting queued data.
The local kit uses its own local service for CLI, MCP and Monitor.
