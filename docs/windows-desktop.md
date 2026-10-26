# Native Windows source status

Windows support is an additional platform implementation of Shin. It uses the
same Core, profiles, generated-index contracts, Observer and MCP protocol as
macOS and Linux. It does not replace their implementations or qualification.

This is a development source route targeting **Windows x64 on local NTFS**.
Windows distribution qualification is pending. The presence of an installer
recipe, this document, or passing source tests does not identify a qualified
Windows release.

## Current scope

| Area | Native Windows source behavior |
| --- | --- |
| Core and retrieval | Indexing, source discovery and snapshot-bound reads use the shared engine. Windows smoke checks cover a selected native subset of the Core suite. |
| Local Observer, CLI and MCP | Native processes and authenticated named pipes; user runtimes require unelevated tokens. Explicit project/root authority and output bounds remain enforced. |
| Storage and workers | Windows file identities, owner-private ACLs, atomic replacement and bounded Job Objects. UNC paths and reparse-point traversal are refused. |
| Monitor | Win32/DX11 host with the shared EN/RU presentation and diagnostics. Source render checks include documentation warnings in project lists and details. |
| Client configuration | `mcp-config` exports configuration for manual review. Automatic `client-connect` editing and graphical project onboarding are not enabled. |
| Terminal UI, usage and updating | `shin-ce stats`, automatic client-history collection and in-app updating remain pending on Windows. Use the native Monitor or text/JSON status where applicable. |
| Distribution | ZIP/WiX MSI recipes and qualification tests exist; development presets leave packaging disabled. Installed lifecycle and exact-archive checks remain separate gates. |

Runtime binaries do not require WSL, Bash, Python, Node or a model service.
Python and the native compiler are development/test dependencies. WSL builds
exercise the Linux implementation and do not substitute for native Windows tests.

## Build and verification

Use the matching checkout's `docs/building.md` for PowerShell commands and full
prerequisites. The [repository build guide](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/building.md)
also describes the distinction between local and hosted-runner checks.

- `windows-native` builds Core and native Observer tools; its
  `windows-native-smoke` test preset selects native Core checks.
- `windows-desktop`, from `observer/`, builds Observer/CLI/MCP and Monitor;
  its full test preset runs all checks registered in that configuration.
- `windows-desktop-smoke` is the hosted-runner subset. It does not include
  interactive desktop, full user-service lifecycle or protected-image checks.

Keep the checkout and an absolute external `SHIN_BUILD_ROOT` outside AppData
and TEMP. Full local tests need an unelevated interactive desktop, symlink
creation rights for refusal fixtures, and a protected CMake installation. Do not substitute an
elevated shell for the user-runtime checks. The source workflow uploads test
reports; it does not publish Windows packages.

The shared refresh regressions cover initial bootstrap, refresh and cached
queries when documentation has a broken local link, warning retention and
clearing after repair. Only the known link-only bootstrap warning is tolerated;
permanent profiles and other safety, freshness and integrity failures remain
strict. Tests use independent disposable state rather than installed projects.

## Runtime and installation boundaries

A source build does not install Shin, register an SCM service or enroll a real
project. Development CLI use must select its own project and data home; a
project's sources and its private index/history must remain separate. Use the
same matching binaries and home for CLI, MCP and Monitor.

The Windows MSI design installs a machine-owned `ShinObserverHost` supervisor
under LocalSystem. That host starts separate unelevated runtimes for existing
and newly signed-in user sessions. Project observation, indexes and queries
belong to those user runtimes. The native service-definition command describes
this model; it does not prove that a matching service is installed or healthy.

Monitor starts separately. Closing its window does not stop an already running
Observer. Installation does not enroll projects or rewrite client configuration.
Project sources, indexes, history and credentials are outside the MSI payload
and must be preserved during maintenance and removal.

## Remaining distribution gates

Windows release work still requires qualification of the exact extracted
archive, matching component versions, runtime dependencies, notices and portable
documentation. The MSI route additionally needs installation, repair, upgrade,
removal, multi-user SCM lifecycle and preserved-data checks in disposable
environments. Its current recipe requires CPack 4.3+ and WiX 4.

The MSI recipe currently derives a numeric package version from the release
prefix. Distinct development versions therefore need an explicit MSI upgrade
version policy before preview-to-preview upgrades can be claimed.

Existing Linux/macOS release gates and the guarded publisher remain independent
requirements. Source checks, archive checks and installed-service checks are
different evidence; none substitutes for the others.
