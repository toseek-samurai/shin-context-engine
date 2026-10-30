# Homebrew installation

The project-owned tap distributes the complete macOS preview: Core, Observer,
CLI, MCP, usage collector, Sand TUI and Shin Monitor. The first package builds
from a pinned source release on your Mac. It needs Homebrew's build tools and
can take several minutes; no Python or Node runtime is required afterward.
Local compilation does not require an Apple Developer Program membership.
The current package targets macOS 15 or newer and is tested on Apple Silicon.

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

Homebrew may ask you to trust this third-party formula. Trust the requested
formula rather than an entire tap when that is sufficient. The tap is maintained
by the Shin project and is not an official Homebrew catalog entry.

The installed commands are `shin-context`, `shin-ce`, `shin-observer`, `shin-mcp`,
`shin-usage` and `shin-monitor`. MCP configurations and default profile bindings
use the stable `opt` prefix, not a particular Cellar version.

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

Close Monitor before upgrading:

```sh
brew update
brew upgrade toseek-samurai/tap/shin-context-engine
```

Restart the background service. For a Homebrew-managed login service:

```sh
brew services restart toseek-samurai/tap/shin-context-engine
shin-monitor
```

For a session service, stop it before upgrading and run `shin-ce service-start`
afterward. Reconnect persistent MCP clients after an upgrade so they start the
new adapter. Their saved executable path remains stable; copying JSON again is
unnecessary.

Homebrew owns package files. Monitor explains the Homebrew command instead of
invoking the native Updater, and `shin-ce update-*`, native installation and
native login-service registration are refused. Project freshness, publication
authority and strict profile checks remain unchanged. A new generator or a
changed profile may still require project-owned review; package installation is
not permission to weaken those checks.

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
formula and its install tests. Publish a recipe only for a completed qualified
release, pin the source archive and dependency resources with SHA-256, and check
install, query, MCP, EN/RU terminal startup, upgrade/reinstall and uninstall.
Do not run a native installer from formula hooks, start services or edit client
settings during installation, fetch dependencies from moving branches, or
silently change project approval policy during `brew upgrade`.
