# Homebrew installation

The [Homebrew tap](https://github.com/toseek-samurai/homebrew-tap) pins a published
source preview. After installing, check the version selected by the tap with
`brew info toseek-samurai/tap/shin-context-engine`. Use `brew upgrade` to install
the version selected by the formula.

The project-owned tap distributes the complete macOS preview: Core, Observer,
CLI, MCP, usage collector, Sand TUI and Shin Monitor. It builds from the pinned
source release on your Mac. It needs Homebrew's build tools and
can take several minutes; no Python or Node runtime is required afterward.
The selected Apple toolchain must provide C++20 `std::jthread` and `std::stop_token`:
use Command Line Tools for Xcode 26 or newer, or a current full Xcode installation.
On macOS 15, apply current system updates if those tools are unavailable. The formula
checks this before compiling the engine and explains how to update an older toolchain.
If Homebrew requests full Xcode while older CLT are installed, updating the CLT to
version 26 or newer also satisfies the requirements. With older CLT and current
Xcode installed together, the formula selects the current Xcode SDK.
Local compilation needs no Apple Developer Program membership. The package
targets macOS 15 or newer. The tap supplies source archives and Homebrew can use
available bottles for dependencies. See the tap's [installation workflow](https://github.com/toseek-samurai/homebrew-tap/actions/workflows/tests.yml)
for the current Apple Silicon and Intel results.

## Install and start

```sh
brew install toseek-samurai/tap/shin-context-engine

cd /path/to/project
shin-ce init
shin-monitor
```

`shin-monitor` opens the tray application. `shin-ce stats` opens the terminal
monitor. Both interfaces support English and Russian. Use Monitor's project setup
to select detected agents and connect them, or follow the
[CLI connection procedure](native-client-setup.md#automatic-setup).
Existing agent settings are preserved by the native review/apply flow.
[Claude Code setup](native-client-setup.md#claude-code) is available with the
dev.135 formula or newer.

The installed commands are `shin-context`, `shin-ce`, `shin-observer`, `shin-mcp`,
`shin-usage` and `shin-monitor`. MCP configurations and default profile bindings
use the stable `opt` prefix, not a particular Cellar version.

### Tap trust

Homebrew 6 and later require trust for formulae from third-party taps.
Installing with the full formula name above grants trust to the Shin
formula. If you already added the tap and a command reports that it is
untrusted, trust the formula and retry the command.

```sh
brew trust --formula toseek-samurai/tap/shin-context-engine
```

You can also trust the entire tap with `brew trust toseek-samurai/tap`.
Homebrew can then load every current and future formula, cask and external
command from that tap. [Homebrew tap trust guide](https://docs.brew.sh/Tap-Trust).

## Background service and login

`init` normally starts a session service. To have Homebrew manage background
startup at login, stop that session service first:

```sh
shin-ce service-stop
brew services start toseek-samurai/tap/shin-context-engine
```

This starts the background service, not Monitor. Open Monitor with `shin-monitor`.
Monitor's native-kit login registration is unavailable for the Homebrew package;
it would bind a versioned application path. No login item is added by installation.

## Upgrade

Quit Monitor and TUI before upgrading. If you run a session service, stop it
first with `shin-ce service-stop`.

```sh
brew update
brew upgrade toseek-samurai/tap/shin-context-engine
```

Restart the background service. For a Homebrew-managed login service:

```sh
brew services restart toseek-samurai/tap/shin-context-engine
shin-monitor
```

For a session service, run `shin-ce service-start` afterward. Reconnect an older
MCP integration if the client reports a version mismatch. Its saved executable
path stays under the stable Homebrew opt prefix.

Homebrew owns package files. Monitor explains the Homebrew command instead of
invoking the native Updater, and `shin-ce update-*`, native installation and
native login-service registration are refused. The development source rebuilds
managed indexes from enrolled folders using project `.gitignore` files and optional root `.shinignore` overrides.
Each agent keeps its existing grants. Source freshness and publication authority
are checked before serving new evidence.

## Remove

Quit Monitor and stop the background service, then remove the package:

```sh
brew services stop toseek-samurai/tap/shin-context-engine
brew uninstall toseek-samurai/tap/shin-context-engine
```

For a session service, use `shin-ce service-stop` before removal instead.
Uninstalling the package preserves project sources, AI client settings and the
private data home (`~/.shin-ce` by default). Remove individual projects through
Shin before uninstalling if you also want their connections detached. Package
removal is not an instruction to delete source files or retained history.

## Existing native kit

The [native kit](local-kit.md) and the Homebrew package have different owners and
update procedures. Installing the formula does not adopt, replace or migrate an
existing native kit. Do not let two installations manage the same data home or
canonical indexes. Shell `PATH` may still select an existing `~/.local/bin/shin-ce`;
`"$(brew --prefix shin-context-engine)/bin/shin-ce" --version` identifies the brew
package explicitly. A migration of an existing working kit needs a separately
reviewed handover, preserving its bindings, history and publication grants.

## Tap maintenance

The [tap repository](https://github.com/toseek-samurai/homebrew-tap) owns the
formula and its install tests. Its **Update preview formula** workflow checks
upstream hourly, or runs manually from main with an optional preview tag. It
selects a published preview with its complete macOS qualification receipt, verifies
the source commit, and derives the source checksum and dependency pins from that
tag. Both Apple Silicon and Intel installation checks must pass before its own
`GITHUB_TOKEN` advances the formula. Normal preview releases need no manual
formula edits or additional personal token. Automation changes receive lightweight
guard checks; the existing macOS lifecycle matrix runs for an actual candidate,
without rebuilding the unchanged old formula for documentation or tooling edits.

Publish a recipe only for a completed qualified release, pin the source archive
and dependency resources with SHA-256, and check install, query, MCP, EN/RU
terminal startup, upgrade/reinstall and uninstall. Changed toolchain requirements
or a new dependency declaration layout still need maintainer review.
Do not run a native installer from formula hooks, start services or edit client
settings during installation, fetch dependencies from moving branches, or
silently change project approval policy during `brew upgrade`.

Keep the `shin_re2` and `shin_abseil` source tags and SHA-256 pins aligned with
[SecretScanner.cmake](../shared/cmake/SecretScanner.cmake). The
formula stages them before offline CMake configuration. `brew test` checks
that both licenses ship with the package.

For a release containing the libuv coordinator, also stage `shin_libuv` from
[Coordinator.cmake](../shared/cmake/Coordinator.cmake) with its exact tag and hash
before offline configuration. Retain both `LICENSE` and `LICENSE-extra` in
`share/shin-observer/licenses/libuv/`; update the published formula only after
that release has passed its package gates.

SQLite is built from the pinned amalgamation in
[SQLite.cmake](../shared/cmake/SQLite.cmake), staged as `shin_sqlite`. Synchronization
verifies the official archive against that source's SHA3-256 pin before deriving
the SHA-256 used by Homebrew. The formula needs no system SQLite dependency.
The retired Server's `shin_httplib` resource is removed when preparing the new recipe;
unrecognized maintained resources still require review.
