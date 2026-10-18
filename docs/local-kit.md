# Local macOS kit

The kit includes Core, Observer, the `shin-ce` CLI, `shin-mcp`, the usage collector,
Shin Monitor, profiles, contracts, and dependency notices. Local retrieval needs
no account or AI model service.
The current development preview targets macOS 15+ on Apple Silicon. It uses an
ad-hoc signature, without Developer ID or notarization; first launch on another
Mac may require manual approval. This is not a stable release.

## Download a preview release

Use the assets attached to the [versioned preview release](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.9).
The release contains the original kit ZIP, its SHA-256 file, and `install.sh`.
Public release downloads do not require a GitHub account.

For one-command installation or update:

```sh
curl -fsSL https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.9/install.sh | sh
```

You can download `install.sh`, inspect it, then run it with `sh install.sh` instead.
It uses macOS tools, checks the archive checksum, and delegates to the same native
`install`/`update` commands documented below. No build tools, Python or Homebrew
are needed. It retains macOS security settings and does not remove quarantine
attributes. Existing manual installations require explicit adoption; follow the
[legacy migration procedure](#adopt-a-previous-manually-installed-kit).

To use custom locations, run `sh install.sh --prefix DIR --bin-dir DIR --home DIR`.
An existing native installation normally needs only `--prefix`; its recorded home
and command links are retained. First installation does not enroll projects or
enable login startup. Repeating the versioned bootstrap safely resumes an update
with the same archive contents.

## Download a CI candidate

After a successful [Source qualification workflow run](https://github.com/toseek-samurai/shin-context-engine/actions/workflows/ci.yml),
open its **Artifacts** section and download `shin-local-macos-<arch>-<run>-<attempt>`.
GitHub requires you to sign in to download Actions artifacts. These development
candidates expire after 14 days; they are not published GitHub Releases.

Extract the downloaded artifact wrapper to get the original kit ZIP and its
`.zip.sha256` file. Keep both files together and verify the checksum before
extracting the kit itself:

```sh
cd /path/to/downloaded-artifact
shasum -a 256 -c shin-local-*.zip.sha256
```

The original kit ZIP retains the executable permissions required by the
installer and Monitor. It is uploaded only after the macOS package checks and
checksum verification pass.
`test-reports-<os>-<component>-<run>-<attempt>` artifacts contain the available
CTest reports and logs, including failed test runs, with the same 14-day retention.

## Install and connect your first project

Extract the complete reviewed ZIP. In Terminal, enter its extracted directory:

```sh
./bin/shin-ce install
export PATH="$HOME/.local/bin:$PATH"
shin-ce installation

cd /path/to/your/project
shin-ce init
shin-ce status --text
shin-ce query "Where are requests validated?"
```

No `sudo` is required. Installation creates command links and records its data
home. It leaves shell startup files and AI client settings unchanged. Add
`$HOME/.local/bin` to your shell's `PATH` yourself if you want these short commands
in future terminals; the installed binaries also work by absolute path.

| Location | Contents |
| --- | --- |
| `~/.local/opt/shin-local` | Application and native binaries |
| `~/.local/bin` | Command links |
| `~/.shin-ce` | Private bindings, indexes, service state, and history |

`init` uses the current project directory and normally starts the local service.
Wait for search readiness in `status --text`; on a large first import, inspect
`service-status --text` for preparation or errors. `init --no-service` creates
the setup without starting the service. Adding a project does not modify its
source files.

Open the menu bar application:

```sh
open "$HOME/.local/opt/shin-local/Shin Monitor.app"
```

Monitor supports English and Russian. Use the language selector at the top of
the panel; restart Monitor to apply it to all native windows and menus. System
uses the operating system language, with English as fallback. Translations are
compiled into the kit and need no downloads.

Click the Shin icon in the menu bar. Add more projects with the folder picker or
`shin-ce project-add --root /path/to/another/project`.

For an interactive terminal dashboard, run `shin-ce stats`. Use Tab or 1–5 for
Overview, Projects, Timing, Storage and Models; Enter opens project details,
`?` shows keyboard help and `q` exits. The dashboard observes the existing
service. In the next preview (dev.12), model counters are collected automatically
for enrolled projects and shown by default in both Monitor and the TUI. The first
pass starts in the background; enrollment changes are checked within five seconds,
and counters refresh every five minutes. Missing or unreadable client history is
reported as incomplete, not as proof of zero usage. Storage inspection happens on demand;
management and cleanup remain explicit CLI commands.

The installer refuses an unowned destination, conflicting command links, or an
existing data home it cannot safely adopt. Read the reported error and choose
fresh locations; do not delete existing state to force installation.

### Automatic model statistics (dev.12)

`init` and `project-add` enroll the project in both local retrieval and background
model statistics. Existing enrolled projects are picked up when the new service
starts. No separate project list or scheduled collector is needed. Statistics
stay under the kit's recorded data home, in `usage/state.json`.

The collector reads local numerical counters from Codex, OpenCode and Hermes;
it does not upload history, contact providers or modify client settings. It uses
standard user locations and respects `CODEX_HOME`, `XDG_DATA_HOME` and `HERMES_HOME`
from the service's environment. An OS login service may have a different environment
from your terminal. Command Code and IDE-specific usage histories are not supported.
Token counts are client-reported usage, not a count of Shin calls or a bill.

Repeating enrollment does not duplicate counters. Disconnecting a project stops
its automatic collection while retaining the last collected statistics. Reconnecting
recollects the group without summing cumulative counters twice. The collector retains
its existing bounds, including 24 usage groups; exhausted capacity is reported and
requires review rather than silently deleting old groups.

An explicit `--usage-file` or Monitor's **Model history…** selection still overrides
the automatic snapshot. For an existing custom collector, stop it before migrating
and preserve its files. Before the first automatic collection, import its group
names, root aliases and source locations once:

```sh
shin-usage --registry /absolute/path/to/shin-home --config /absolute/path/to/usage-config.json
```

This creates a separate automatic snapshot and leaves the original configuration
and history untouched. Import into an already initialized automatic collector is
refused. Stop the Shin service during this initial import to avoid competing writers.
Select the new `usage/state.json` in Monitor if a custom history file was selected.
The imported groups retain their aliases; a group is collected while any of its
roots is enrolled. Historical totals are reconstructed only from readable client
history; retain the old snapshot for any history no longer present in the clients.

## Connect a coding agent

In the next preview (dev.12, available when built from this source), Monitor opens
**Set up Shin** on a fresh installation. Choose the first folder, select agents
and profiles, and apply their MCP settings directly. Existing settings are preserved. You can opt into background service
startup at login from the same window. It does not register Monitor itself.
See [guided client setup](native-client-setup.md#automatic-setup-next-preview-dev11).
The published dev.9 preview uses the CLI instructions above.


Use the installed `shin-mcp` executable with an explicit project root. For a
default installation its path is `~/.local/opt/shin-local/bin/shin-mcp`; expand
`~` to an absolute path in client JSON. It uses the installation's recorded data
home. See [native client setup](native-client-setup.md) for command and argument
examples, including portable extraction and multiple project roots.

## Automatic startup

Starting a service for the current session and registering startup at login are
separate operations. Once the project works, opt into user-service startup with:

```sh
shin-ce service-install
shin-ce service-installation
```

To inspect, stop, or start the local service manually:

```sh
shin-ce service-status --text
shin-ce service-stop
shin-ce service-start
```

`service-install` registers the background service, not the Monitor UI.
`service-uninstall` removes automatic service startup. `uninstall` removes an
owned kit and its command links while preserving project data and history.
Use `shin-ce --help` for current options. Use the update procedure below to replace an installed kit while retaining its
recorded data home and service preference.

## Update an existing kit

Download and verify the new complete kit, then extract it outside the installed
folder. Close Monitor and run the **new kit's** executable:

```sh
./bin/shin-ce update
```

For a custom installation, add `--prefix /path/to/installed-kit`; its recorded
command directory and data home are retained. The updater verifies both manifests,
checks current projects under the new engine, stages complete files, stops the
owned service and atomically exchanges the payload directories. It restores the
previous service/autostart preference and checks readiness before reporting success.
Project bindings, indexes, identity and history remain in their existing data home.
Restart Monitor after completion. Restart persistent MCP connections in your coding
agent as well: already-running adapters retain the old version and cannot query
the new service until relaunched. Client configuration paths stay the same.

An interrupted operation is resumed by repeating the same command from the same
extracted kit. Failed activation restores the previous payload and service; the
error reports whether rollback completed or further recovery is required. One
previous kit is retained outside the active payload. A subsequent update or
uninstall removes it only after verifying its ownership and contents. Unexpected
files or corruption are preserved for review, never silently deleted.

Updating with the exact installed archive is a no-op. A changed archive must have
a newer version; downgrades and altered same-version kits are refused. This first
updater supports the same Core version only: a change to the Core data contract
requires an explicit migration. It does not download releases or change shell,
client, project or model-usage configuration.

### Adopt a previous manually installed kit

Older installations without a native installation receipt require an explicit,
one-time adoption. Supply all three existing locations using the new kit:

```sh
./bin/shin-ce update --adopt-existing \
  --prefix "$HOME/.local/opt/shin-local" \
  --bin-dir "$HOME/.local/bin" \
  --home /absolute/path/to/existing-shin-data
```

Use the actual existing data directory, not a fresh default. Adoption verifies a
complete old kit manifest, its command links, registry, identity and service
ownership before recording installation ownership. It makes the kit directory
private and preserves existing project data. Arbitrary directories, conflicting
links and another installation's data home are refused. Later updates use only
`update` (and `--prefix` if customized).

## Portable use and custom locations

You can run binaries directly from the extracted kit without installing command
links. Select a private data home outside every source root and keep it the same
for CLI and MCP:

```sh
/path/to/extracted-kit/bin/shin-ce init \
  --root /path/to/project --home /path/to/private-data
/path/to/extracted-kit/bin/shin-ce status --text \
  --root /path/to/project --home /path/to/private-data
```

For a custom per-user installation, pass `--prefix`, `--bin-dir`, and `--home` to
`install`. Then use that installation's command links; its data home is pinned.
Existing managed indexes require their owner's explicit authorization before
changing the publisher or binding.

## Build the package

From a macOS source checkout, use a C++20 compiler, CMake 3.24+, SQLite with FTS5
3.38+, OpenSSL Crypto, and the SDK's curses library for the terminal monitor.
Python 3.11+, Bash, and jq are needed for the checks.
CMake downloads pinned dependencies with checked hashes.

```sh
cmake -S observer -B /tmp/shin-observer-kit \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DSHIN_OBSERVER_SOURCE_DISCOVERY=ON \
  -DSHIN_OBSERVER_MCP=ON \
  -DSHIN_OBSERVER_TRAY=ON \
  -DSHIN_OBSERVER_LOCAL_KIT=ON
cmake --build /tmp/shin-observer-kit --parallel 4
ctest --test-dir /tmp/shin-observer-kit --output-on-failure
cpack --config /tmp/shin-observer-kit/CPackConfig.cmake
```

The ZIP and checksum are under the build directory's `packages` folder. Keep the
whole directory layout when extracting. The package contains no credentials,
client settings, or project state. The build does not qualify other CPU
architectures or macOS versions. Stable distribution still needs Developer ID signing/notarization and a
first-launch check on a clean machine; an ad-hoc signature alone does not
establish that experience.


## Core patch compatibility

The dev.13 kit qualifies the explicit Core `2.0.5-dev.1 → 2.0.5-dev.2`
transition. This recall correction preserves generator-v22 evidence and storage
formats. The updater still qualifies every enrolled project and preserves its
ownership and rollback checks; other Core transitions remain refused.
For this transition, stale or validation-blocked publications are refreshed by
the ordinary Observer under existing grants during journaled activation. All
projects must qualify before success. Interrupted recovery resumes from the same
archive; failed activation rolls back the kit and restores the previous service
readiness boundary. Existing private-bootstrap bindings are migrated through native initialization,
without new enrollment or changed identity/profile. Rollback uses the verified
previous kit to restore their previous Core binding. Canonical publisher grants
retain their original identity. No index deletion or profile change is used.
