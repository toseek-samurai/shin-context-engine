# Documentation and platform support

## Current preview

The current published preview is [dev.96](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.96).
It contains Core **2.0.5-dev.18** and Observer/CLI/MCP/Monitor **2.0.5-dev.96**.
Its macOS kit, Windows ZIP/MSI and Ubuntu 24.04 x86_64 terminal archive have each
passed their platform's guarded publication checks. Each download group has a
`shin-preview-PLATFORM.json` receipt binding its files to the qualified source,
CI run, attempt and SHA-256 values.
Core and Observer are versioned independently; `shin-context --version` reports
the generator, and `shin-ce --version` reports both component versions.
There is no stable public release yet.

Source identities are maintained in [VERSION](../VERSION),
[observer/VERSION](../observer/VERSION) and the
[development policy](machine/shin-development-v1.json). An installed process can
still belong to an earlier kit. Compatible MCP connections resume automatically;
reconnect an older integration if it reports a version mismatch. IDE-hosted
clients have their own sessions; restarting another desktop client does not
restart them.

## Desktop layout

Monitor groups tasks under **Overview**, **Projects**, **History** and
**Settings**, with labeled actions and technical details disclosed separately.
History opens with recorded calls; timing and optional agent usage remain
secondary views. Published dev.85 introduced this navigation with Light Sand.

The current preview uses the original dark Sand palette while
keeping the new layout, 14 px body text and 520 px standard width. Short Overview,
Projects and Settings pages fit their contents up to a 680 px ceiling; long
history and detail views retain bounded height and scrolling.
The [native screenshots](images/README.md) identify the
source and capture boundaries. The terminal keeps its existing Sand appearance
and Activity navigation.

## Capabilities by platform

This table describes the published platform routes, including Linux terminal tools. A shared engine or
dashboard does not imply that every platform has the same setup and update tools.

| Capability | macOS native kit | macOS Homebrew | Linux / WSL2 terminal tools | Windows x64 ZIP / MSI |
| --- | --- | --- | --- | --- |
| Core, local Observer, CLI and MCP | Yes | Yes | Yes | Yes |
| Desktop Monitor: Overview, Projects, History, Settings; Light Sand; EN/RU | Yes | Yes | No native desktop host | Yes |
| TUI `shin-ce stats`, line charts and EN/RU | Yes | Yes | Yes | Not implemented |
| Add a project from Monitor | Yes | Yes | Use CLI | Yes |
| Automatic MCP client configuration | Reviewed CLI / Monitor setup | Reviewed CLI / Monitor setup | Reviewed CLI setup | Reviewed CLI / Monitor setup |
| Automatic agent token-history collection | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes | Codex, OpenCode, Hermes |
| Storage management | Monitor and CLI | Monitor and CLI | CLI; TUI inspection | Monitor and CLI |
| Release checks | Automatic or manual | Homebrew commands | No automatic release download | Automatic or manual in Monitor |
| Update installation | Independent Updater, Monitor or CLI/TUI | `brew upgrade` | Replace the terminal archive or rebuild source tools | Reviewed MSI installation with UAC |
| Background startup at login | Opt-in native service registration | `brew services` | No built-in OS service registration | MSI machine host supervises unelevated user runtimes |

Token totals from agent history are separate from MCP request statistics.
Missing or unavailable client history does not mean zero usage. Request timing
and the session journal remain available independently of agent token history.

The native binary kit targets **macOS 15+ on Apple Silicon**. Homebrew builds the
complete application from source and has passed its installation checks on Apple
Silicon; Intel installation qualification is incomplete. Linux/WSL2 currently
offers the published Ubuntu 24.04 x86_64 terminal archive or source builds. The
archive includes Core, Observer, CLI, MCP, usage collection and the EN/RU TUI; it
requires glibc 2.39+, libstdc++6, OpenSSL 3 and SQLite system packages. It includes
no Linux desktop host, OS service installer or automatic updater. See
[Linux and WSL2 tools](building.md#linux-and-wsl2-terminal-tools).
Windows targets **x64 on local NTFS**; WSL tests exercise the
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
