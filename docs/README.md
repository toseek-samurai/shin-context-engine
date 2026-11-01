# Documentation and platform support

## Current preview

The current published preview is [dev.77](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.77).
It contains Core **2.0.5-dev.13** and Observer/CLI/MCP/Monitor **2.0.5-dev.77**.
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

This table describes the published platform routes and the Linux source route. A shared engine or
dashboard does not imply that every platform has the same setup and update tools.

| Capability | macOS native kit | macOS Homebrew | Linux / WSL2 source build | Windows x64 ZIP / MSI |
| --- | --- | --- | --- | --- |
| Core, local Observer, CLI and MCP | Yes | Yes | Yes | Yes |
| Desktop Monitor: Overview, Projects, Activity, Settings; Sand; EN/RU | Yes | Yes | No native desktop host | Yes |
| TUI `shin-ce stats`, line charts and EN/RU | Yes | Yes | Yes | Not implemented |
| Add a project from Monitor | Yes | Yes | Use CLI | Yes |
| Automatic MCP client configuration | Reviewed CLI / Monitor setup | Reviewed CLI / Monitor setup | Reviewed CLI setup | Reviewed CLI / Monitor setup |
| Automatic agent token-history collection | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes |
| Storage management | Monitor and CLI | Monitor and CLI | CLI; TUI inspection | Monitor and CLI |
| Release checks | Automatic or manual | Homebrew commands | No automatic release download | Automatic or manual in Monitor |
| Update installation | Independent Updater, Monitor or CLI/TUI | `brew upgrade` | Rebuild and replace source-built tools | Reviewed MSI installation with UAC |
| Background startup at login | Opt-in native service registration | `brew services` | No built-in OS service registration | MSI machine host supervises unelevated user runtimes |

Token totals from agent history are separate from MCP request statistics.
Missing or unavailable client history does not mean zero usage. Request timing
and the session journal remain available independently of agent token history.

The native binary kit targets **macOS 15+ on Apple Silicon**. Homebrew builds the
complete application from source and has passed its installation checks on Apple
Silicon; Intel installation qualification is incomplete. Linux/WSL2 currently
uses source builds. Source CI can also prepare an independently qualified Ubuntu
24.04 x86_64 terminal archive; use it only when that release includes the asset.
This packaging route is prepared for the next preview and does not add a Linux
desktop host or automatic updater. Windows targets **x64 on local NTFS**; WSL tests exercise the
Linux implementation, not the native Windows application.

The macOS preview is ad-hoc signed, without Developer ID or notarization.
Homebrew is a project-owned tap, not an entry in Homebrew's official catalog.

## Windows Monitor

Preview dev.74 includes graphical project/agent setup, project actions, storage
management, startup-settings access, automatic client-history collection and
reviewed MSI installation with UAC. Project preparation follows confirmed MSI
success using the new installed CLI. See [setup and updates](windows-desktop.md#monitor-setup-and-updates)
for the wizard Finish action, saved results and unattended installation boundaries.

## Installation and everyday use

- [macOS native kit](local-kit.md): installation, startup, updating and recovery.
- [Homebrew](homebrew.md): full macOS source package and its separate lifecycle.
- [Windows desktop preview](windows-desktop.md): ZIP/MSI, setup, reviewed updates and platform limits.
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
different things. Each guarded platform publisher requires that platform's
matching source jobs and exact package proof; another platform's pending runner
does not block qualified downloads. Hosted Windows renders use software D3D11
without Explorer and do not certify notification-area behavior or an installed
MSI lifecycle. Screenshot fixtures use synthetic data; they are not measurements
of a live installation. See each platform guide for its verification scope.
