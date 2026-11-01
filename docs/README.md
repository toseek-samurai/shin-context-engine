# Documentation and platform support

## Current preview

The current published preview is [dev.70](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.70).
It contains Core **2.0.5-dev.13** and Observer/CLI/MCP/Monitor **2.0.5-dev.70**.
Core and Observer are versioned independently; `shin-context --version` reports
the generator, and `shin-ce --version` reports both component versions.
There is no stable public release yet.

Source identities are maintained in [VERSION](../VERSION),
[observer/VERSION](../observer/VERSION) and the
[development policy](machine/shin-development-v1.json). An installed process can
still belong to an earlier kit: reconnect persistent MCP clients after updating,
including clients hosted inside an IDE. Restarting another desktop client does
not restart those IDE sessions.

## Capabilities by platform

This table describes the current preview and source routes. A shared engine or
dashboard does not imply that every platform has the same setup and update tools.

| Capability | macOS native kit | macOS Homebrew | Linux / WSL2 source build | Windows x64 ZIP / MSI |
| --- | --- | --- | --- | --- |
| Core, local Observer, CLI and MCP | Yes | Yes | Yes | Yes |
| Desktop Monitor: Overview, Projects, Activity, Settings; Sand; EN/RU | Yes | Yes | No native desktop host | Yes |
| TUI `shin-ce stats`, line charts and EN/RU | Yes | Yes | Yes | Not implemented |
| Add a project from Monitor | Yes | Yes | Use CLI | Use CLI |
| Automatic MCP client configuration | Reviewed CLI / Monitor setup | Reviewed CLI / Monitor setup | Reviewed CLI setup | Manual configuration exported by `mcp-config` |
| Automatic agent token-history collection | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Not implemented; an existing normalized snapshot can be selected |
| Storage management | Monitor and CLI | Monitor and CLI | CLI; TUI inspection | CLI; no management window |
| Release checks | Automatic or manual | Homebrew commands | No automatic release download | Manual check in Monitor |
| Update installation | Independent Updater, Monitor or CLI/TUI | `brew upgrade` | Rebuild and replace source-built tools | Download and run the MSI manually |
| Background startup at login | Opt-in native service registration | `brew services` | No built-in OS service registration | MSI machine host supervises unelevated user runtimes |

Token totals from agent history are separate from MCP request statistics.
Missing history does not mean zero usage. A Windows Monitor can show Shin's
request timing and journal without an automatic agent-history collector.

The native binary kit targets **macOS 15+ on Apple Silicon**. Homebrew builds the
complete application from source and has passed its installation checks on Apple
Silicon; Intel installation qualification is incomplete. Linux/WSL2 currently
uses source builds. Windows targets **x64 on local NTFS**; WSL tests exercise the
Linux implementation, not the native Windows application.

The macOS preview is ad-hoc signed, without Developer ID or notarization.
Homebrew is a project-owned tap, not an entry in Homebrew's official catalog.

## Installation and everyday use

- [macOS native kit](local-kit.md): installation, startup, updating and recovery.
- [Homebrew](homebrew.md): full macOS source package and its separate lifecycle.
- [Windows desktop preview](windows-desktop.md): ZIP/MSI, manual update checks and platform limits.
- [Build from source](building.md): Core, Linux/WSL2 and native Windows development.
- [Connect a coding agent](native-client-setup.md): automatic setup where supported and manual MCP examples.
- [CLI and diagnostics](cli.md): readiness, source reads, statistics and recovery.
- [Terminal gallery](tui.md) and [screenshot provenance](images/README.md).

## Development and evidence

- [Architecture](architecture.md), [contributing](../CONTRIBUTING.md) and [test ownership](../tests/README.md).
- [Agent maintenance](agent-maintenance.md): profiles, authority and verification after refactors.
- [Machine-readable contracts](machine/README.md): source identities and protocol boundaries.
- [Third-party notices](../THIRD_PARTY.md) and [security reporting](../SECURITY.md).

Current guides describe the supported routes above. Files in `releases/` are
historical release or candidate notes: some candidates were never published, and
old installation commands describe their own version. Use the current guides for
installation. Old generator or release identities retained in contracts record
provenance; they do not promise a compatibility reader or a supported old kit.

Source tests, exact-package checks and installed interactive tests establish
different things. The guarded publisher requires the matching Linux/macOS and
Windows jobs and exact package proof. Hosted Windows renders use software D3D11
without Explorer and do not certify notification-area behavior or an installed
MSI lifecycle. Screenshot fixtures use synthetic data; they are not measurements
of a live installation. See each platform guide for its verification scope.
