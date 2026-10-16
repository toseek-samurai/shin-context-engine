# Architecture and scope

Shin is split into components with distinct responsibilities. The current source identity and release boundaries are recorded in [VERSION](../VERSION) and the [machine-readable product manifest](machine/shin-context-engine.manifest.json).

## Components

**Core** owns profiles, indexing, retrieval, evidence formatting, and the command-line interface. A profile selects the paths and file families that belong in a project context. Use `--out-dir` to keep generated output apart from source; the low-level CLI defaults to the project root when this option is omitted. Core reads do not grant authority to publish or execute anything.

**Observer** owns local project observation, bounded local state, and explicit delivery protocols. Optional native tools include the `shin-ce` project CLI, the stdio `shin-mcp` adapter, usage collection, and the desktop Monitor. Observer can use Core source discovery for local source queries. The optional macOS local kit is described in its packaged `local-kit.md` and `native-client-setup.md`.

**Server** is a separate authenticated service with persistent workflows and PostgreSQL dependencies. It is development source; it is not included in the local macOS kit and should not be treated as a ready shared service.

**Shared code** contains versioned protocols and operating-system support. It does not grant project, publication, or execution authority by itself.

## Authority and state boundaries

Project profiles determine the source scope. Generated indexes and caches are outputs and must be placed separately from source. Observer connections are explicit and project-bound; a read command does not enroll a project or grant publication rights. Existing indexes require an explicit owner decision before management or replacement. Server credentials and persistent state are configured separately from this repository.

The packaged local kit does not configure client credentials, edit client settings, or register an operating-system service during extraction. Optional installation and service commands are per-user operations that a person must choose deliberately. See the packaged local-kit guide for the exact native commands.

## Platform and release scope

The Core source provides a CMake route for macOS and Linux. Observer includes local platform implementations, while the complete native local kit is a macOS package path. Source presence does not qualify other platforms or installers. Windows remains unqualified.

Server source is maintained as a development component with its own dependencies, configuration, database, and qualification gates. A successful source build alone does not establish deployment readiness. Component identities and current qualification state come from the checked-in version files and product manifest; internal or machine-local evidence is not a public release receipt.

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

Core lives in `src/` with its tests in `tests/`. Observer and Server each own
`src/`, `include/`, tests and CMake entry points. `shared/` contains protocol and
platform support; `third_party/` holds vendored dependencies. `profiles/` selects
source scope, and `docs/machine/` holds versioned contracts.

Observer's `ui/` owns the shared status, timing, usage presentation and compiled
EN/RU catalog. `tray/` adapts it to macOS/Windows and `tui/` adapts it to the
terminal. Language preferences never change protocol fields or project data.
The former optional web query panel and its HTTP adapter have been removed;
its numeric history, metrics and portfolio producers remain independent tools.
