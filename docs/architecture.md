# Architecture and scope

Shin is split into components with distinct responsibilities. The current source identity and release boundaries are recorded in [VERSION](../VERSION) and the [machine-readable product manifest](machine/shin-context-engine.manifest.json).

## Components

**Core** owns profiles, indexing, retrieval, evidence formatting, and the command-line interface. A profile selects the paths and file families that belong in a project context. Use `--out-dir` to keep generated output apart from source; the low-level CLI defaults to the project root when this option is omitted. Core reads do not grant authority to publish or execute anything.

**Observer** owns local project observation, bounded local state, and persisted observation formats. Optional native tools include the `shin-ce` project CLI and terminal monitor, the stdio `shin-mcp` adapter, usage collection, and the desktop Monitor. Observer can use Core source discovery for local source queries. The optional macOS local kit is described in its packaged `local-kit.md` and `native-client-setup.md`.

**Shared code** contains versioned protocols and operating-system support. It does not grant project, publication, or execution authority by itself.

## Authority and state boundaries

Project profiles determine the source scope. Generated indexes and caches are outputs and must be placed separately from source. Observer connections are explicit and project-bound; a read command does not enroll a project or grant publication rights. Existing indexes require an explicit owner decision before management or replacement.

Extracting a kit does not configure client credentials, edit client settings, or register an operating-system service. The macOS installation and service commands are explicit per-user operations. The Windows MSI design uses an elevated machine host to supervise separate unelevated user runtimes; its installation and lifecycle qualification are separate from source testing. Project authority remains with the user runtime, not with the UI or installer.

## Platform and release scope

Core and Observer provide CMake source routes for macOS, Linux and native Windows x64. Windows adds adapters to the same engine, profile registry, freshness checks and versioned protocols. Private storage uses Windows file identities and ACL checks, local transport authenticates named-pipe peers, and worker lifetime is bounded through Job Objects. These adapters preserve the shared authority and refusal contracts.

The published local-kit route remains macOS. Windows has development build/test presets and a separate source-check workflow; these do not qualify ZIP/MSI distribution, installed SCM behavior, or feature parity. See [Windows source status](windows-desktop.md) for implemented scope and pending gates, and [building](building.md) for commands. Linux/macOS qualification remains independently required.

Component identities and qualification state come from the checked-in version files and product manifest. Source builds do not qualify a Stable release.

## Current capabilities and agent integration

The architecture exposes search, bounded Brief and task-oriented Pack retrieval,
snapshot-pinned batches, source inspection and reading, and change-aware evidence.
These capabilities share Core's provenance, freshness and output-budget contracts.
Agent integration uses native MCP or an explicit CLI adapter. The agent still owns
task execution and conversation memory; a retrieved instruction cannot grant it
publication or filesystem authority.

## Quality gates and performance limitations

Quality gates combine source/profile validation, generated-index integrity,
declared retrieval cases, Pack completion and resource checks. CTest separately
exercises runtime correctness, isolation, refusal and recovery. A passing
retrieval case is evidence about that declared case, not universal answer quality.

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

Observer's `ui/` owns the shared status, timing, usage presentation and compiled
EN/RU catalog. `tray/` adapts it to macOS/Windows and `tui/` adapts it to the
terminal. Language preferences never change protocol fields or project data.
The macOS/Linux Sand TUI uses statically linked FTXUI for layout, input and line charts.
It reads the existing local service, usage snapshot and storage inventory; opening
it does not enroll projects, start another service or publish an index.
The former optional web query panel and its HTTP adapter have been removed;
its numeric history, metrics and portfolio producers remain independent tools.

## Persisted compatibility

The standalone Server and its deployment tooling have been removed. Observer
retains its existing state, outbox and explicit loopback delivery formats so
removing the backend does not rewrite local history or identity pins. These
compatibility interfaces do not provide a remote service or synchronize source
code. The local kit uses its own local service for CLI, MCP and Monitor.
