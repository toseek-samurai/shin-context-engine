# Local macOS kit

The kit includes Core, Observer, the `shin-ce` CLI, `shin-mcp`, the usage collector,
Shin Monitor, profiles, contracts, and dependency notices. Local retrieval needs
no account or AI model service. Server and PostgreSQL are separate components.
This is a development candidate; public release approval is pending.

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
service. Model counters require `--usage-file /absolute/path/to/usage/state.json`
from the separately configured collector. Storage inspection happens on demand;
management and cleanup remain explicit CLI commands.

The installer refuses an unowned destination, conflicting command links, or an
existing data home it cannot safely adopt. Read the reported error and choose
fresh locations; do not delete existing state to force installation.

## Connect a coding agent

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
Use `shin-ce --help` for current options. Replacing a different installed kit is
not an automatic upgrade flow; preserve its state and follow the reported
installation checks.

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
architectures or macOS versions. Public distribution still needs a defined
minimum macOS version and a signing/notarization and first-launch check on a
clean machine; a local ad-hoc signature alone does not establish that experience.
