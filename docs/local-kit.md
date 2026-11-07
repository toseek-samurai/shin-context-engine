# Local macOS kit

For a full source-built installation managed by Homebrew, use the
[Homebrew guide](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/homebrew.md). Its update and service ownership differs from this native kit.

The kit includes Core, Observer, the `shin-ce` CLI, `shin-mcp`, the usage collector,
Shin Monitor, profiles, contracts, and dependency notices. Local retrieval needs
no account or AI model service.
The current development preview targets macOS 15+ on Apple Silicon. It uses an
ad-hoc signature, without Developer ID or notarization; first launch on another
Mac may require manual approval. This is not a stable release.

This guide covers the macOS kit. Native Windows source builds and the separate,
Windows ZIP/MSI packaging route are described in the [Windows guide](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/windows-desktop.md).

## Download a preview release

Use the assets attached to the [versioned preview release](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.77).
The release contains the kit and independent Updater ZIPs, their SHA-256 files,
and `install.sh`.
Public release downloads do not require a GitHub account.

For one-command installation or update:

```sh
curl -fsSL https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.77/install.sh | sh
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

A project using the default bootstrap profile keeps serving
verified context when local Markdown links are broken. Projects and project details
show a documentation warning, which clears after the corrected sources qualify.
Index integrity, source freshness, secrets and resource checks remain required.
Shin separates retrieval availability from declared search quality
for explicit project profiles. Ordinary eval/Pack misses become a bounded search
warning while a safe, current candidate is published and served. Strict Core
`eval`, `validate` and `ready` verdicts and exit codes remain unchanged for CI.
Source/authority checks, index integrity, malformed configuration, runtime
errors, forbidden-result checks and mandatory resource ceilings still block
admission. No profile or evaluation expectation is rewritten to clear a warning.

Open the menu bar application:

```sh
open "$HOME/.local/opt/shin-local/Shin Monitor.app"
```

Monitor supports English and Russian. Use **Settings → Language**;
it applies immediately, including open native windows and menus. System
uses the operating system language, with English as fallback. Translations are
compiled into the kit and need no downloads.

Click the Shin icon in the menu bar. The update indicator
stays inside the same square icon. macOS can still hide menu-bar extras when
space is tight; it provides no public always-visible priority for an app's icon.
**Shin Monitor.app** runs as a tray application: opening it
shows the popup, and closing the popup leaves Monitor running. The dashboard
stays attached to its popover; it does not detach into a standalone window or
add a Dock icon. If the menu bar is crowded, hold Command and drag the Shin
icon to a visible position. An explicit Quit exits Monitor.

Current development source restores the original dark Sand palette with
**Overview**, **Projects**, **History** and **Settings**. The compact panel is
520 px wide, with 14 px body text and content-dependent height up to 680 px.
Short pages no longer reserve a full-height empty area; long views retain their
height limit and scrolling. These refinements are not yet released. Published
dev.85 uses Light Sand with the same navigation. Earlier desktop previews use
**Activity** as their third section.
Overview summarizes current project readiness and leads to a project needing
review. Projects groups folder and agent connections with their current context
status. Project explanations expose a labeled next action, with search-check
details inside the panel and technical reports disclosed separately. History
opens with the session journal; request timing and agent token history remain
available as secondary views. Agent token totals are distinct from calls to Shin. Timing explicitly selects this service
run or retained history, independently of the overview chart's period. Settings
provides language, storage and update entry points. Controls use readable labels
and compact buttons; status labels remain passive. Agent setup keeps custom
configuration paths under **Advanced agent settings**. The TUI retains its
existing navigation and appearance, with platform-specific setup and update capabilities.

The installed native-kit Monitor registers itself with macOS Login Items
on its first launch. Use **Launch Monitor at login** in its app or
status-item menu to disable or re-enable this. The checkmark reads the actual
macOS setting; external opt-out is preserved. If approval is needed, the menu
opens Login Items in System Settings. Development and portable builds do not
register themselves. Login launches stay in the background; an explicit open
shows the dashboard. Uninstall removes the registration owned by that bundle.
The background service has a separate login-startup setting.

Add more projects from **Projects** with the folder picker or
`shin-ce project-add --root /path/to/another/project`.

For the dark Sand terminal dashboard with line charts, run `shin-ce stats`.
The TUI groups its screens under Overview, Projects, Activity and
Settings (`1`–`4`, Tab / Shift-Tab). Left/Right or `[` / `]` switches pages within
Activity and Settings. Activity includes request timing, agents and the session
journal, including explicit host context markers when available.
Settings includes **Updates** (shortcut `u`): `r` checks releases, `i` reviews
installation, `y` confirms and `n` or Esc cancels. Its progress and saved results
use the same native updater as Monitor. Automatic release download is macOS-only.
Enter opens project details,
`?` shows keyboard help and `q` exits. The dashboard observes the existing
service. Model counters are collected automatically for enrolled projects.
The desktop redesign shows them under **History → Agents**; the TUI and earlier dark
Monitor previews keep **Activity → Agents**. Both use the automatic snapshot by
default. The first
pass starts in the background; enrollment changes are checked within five seconds,
and counters refresh every five minutes. Missing or unreadable client history is
reported as incomplete, not as proof of zero usage. Storage inspection happens on demand;
management and cleanup remain explicit CLI commands.
Click a metric or press `g` to change its graph; `h` cycles Live, one hour and
24 hours. `L` switches EN/RU immediately. An 80-column terminal uses a compact
layout; project readiness and partial coverage remain distinct.

The installer refuses an unowned destination, conflicting command links, or an
existing data home it cannot safely adopt. Read the reported error and choose
fresh locations; do not delete existing state to force installation.

### Project source budgets

New projects connected through `init` or `project-add` use the bundled
`generic-source` profile, which explicitly allows 30,000 source files and 2 MiB
per file. The language-specific bootstrap profiles allow 10,000 files and 1 MiB
per file. These are extraction budgets, not the amount of text sent to an agent
or a guarantee about peak indexing memory.

Private bootstrap indexes have a 256 MiB aggregate load budget; an individual
generated artifact may use that budget, including a large facts or terms file.
This is separate from the 2 MiB limit on an individual source file. Project-owned
Observer configurations keep their explicit snapshot and artifact limits. An
unfinished first import can be retried with the current bootstrap template;
prepared projects retain their pinned profile revision until an approved update.
Source discovery follows the profile's file count and per-file size budgets;
its 64 MiB aggregate source-text and 256 MiB cache budgets remain separate.
Sources between 1 and 2 MiB use bounded text windows for discovery, while the
syntax parser retains its 1 MiB, node-count and progress limits.

Core defaults to 10,000 files and 1 MiB per file
when a profile omits those fields, matching the language-specific templates.
The bundled new-project template declares its budgets explicitly. An existing
permanent project keeps its explicit reviewed limits. `profile-policy` reports
the effective values and whether each came from the profile or Core. Check that
output for the installed kit; explicit project limits take precedence over defaults.

Set deliberate budgets in a permanent profile and approve its exact revision
through the native profile-review/approval workflow. Let the existing managed
publisher validate the new snapshot, then verify readiness and the observed,
capped and skipped source counts. Increasing a budget does not approve changed
roots, waive quality gates or make a previous snapshot current. Avoid removing
budgets: size, validation time and memory still need independent bounds.

### Moving an existing permanent profile

Keep project-specific profiles with their project, for example under
`profiles/`. Choose a directory outside the profile's selected document and source
roots so the configuration does not become retrieval evidence itself.
The generated index and local history remain in
their existing external storage. This does not make Shin a project build dependency.

`profile-relocate` moves an enrolled, active,
managed route to an existing directory containing an exact copy of its approved
`PROFILE.json`. Stop the local service first, then run:

```sh
shin-ce profile-relocate --root /path/to/project \
  --profile-dir /path/to/project/profiles
shin-ce service-start
```

The command preserves publisher permission, project identities, approvals,
profile revision history and index contents. It changes the profile directory
in the Observer configuration and its dependent binding records. A private
journal supports resuming the same command after interruption; unknown edits
or replaced directories require review and are never overwritten.

Relocation alone does not certify readiness. The managed publisher detects
the changed profile provenance, validates and publishes the matching generation.
Verify project readiness before deleting the old profile copy. To reverse a
completed move while both copies remain available, stop the service and use
the same command with the previous directory. Repeated completed requests
for the current directory are harmless.

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

An explicit `--usage-file` or Monitor's agent-history source chooser in **Settings** overrides
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

Monitor opens **Set up Shin** on a fresh installation. Choose the first folder, select agents
and profiles, and apply their MCP settings directly. Existing settings are preserved. You can opt into background service
startup at login from the same window. It does not register Monitor itself.
See [guided client setup](native-client-setup.md#automatic-setup).


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

Monitor always displays its installed version, including while offline.
The current layout keeps it in the fixed footer; previews before the new
navigation show it beneath the panel title. An available newer version appears separately in
the update offer.

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
Terminal updates show each stage and elapsed time on stderr, with real byte counts
for copying and completed/total project counts for qualification. stdout retains
the JSON receipt. Redirected commands stay quiet by default; add `--progress` to
include readable stages in a log. No desktop window opens for a terminal command.
Restart Monitor after completion. Current development source adds an update-aware
MCP bridge on macOS: once a client has loaded this bridge, compatible kit updates
do not require restarting its MCP connection or coding application. Older adapters
need one reconnection to load it. Published kits without the new runtime manifest
retain their existing reconnect requirement. Client configuration paths stay the same.
The bridge verifies the current code inventory, exact Observer/Core versions and
runtime digest; the service must belong to that exact cohort. It never accepts a
foreign-version success. An automatically checked adapter-code fingerprint also
prevents retaining stale tools, session framing or journal logic. A changed MCP tools/transport contract explicitly requires
reconnecting the Shin integration. A service outage during installation remains a
temporary refusal, not permission to run a second service or re-enroll projects.
Windows MSI process replacement retains its separate reconnect instructions.

An interrupted operation is resumed by repeating the same command from the same
extracted kit. Failed activation restores the previous payload and service; the
error reports whether rollback completed or further recovery is required. One
previous kit is retained outside the active payload. A subsequent update or
uninstall removes it only after verifying its ownership and contents. Unexpected
files or corruption are preserved for review, never silently deleted.

The current preview uses Core dev.13 and the Objective-C++ v24 index format. It
rebuilds older derived indexes from approved project sources under their existing
authority; no old retrieval reader or data conversion is retained. During rollback to an
older kit, a private bootstrap index that the old CLI cannot read is atomically
retired through the owned temporary-publication lifecycle before the old CLI
builds it again. The installation journal retains pending recovery throughout.
Bindings, profiles, sources, grants, identity and history are not deleted.

Failed activation retains bounded
enrollment error codes and opaque project hashes in `error.diagnostic` and
`shin-ce installation` (`last_update_diagnostic`). Rollback checks the previous
service independently if its startup acknowledgement fails. A restored service
may have temporarily blocked projects: `last_rollback.service_restored` then stays
separate from `all_projects_ready` and `pending_projects`. These fields describe
the rollback observation, not current readiness; use `service-status` and project
`status` for the current state. Identity, integrity and registry failures still
leave recovery incomplete. The updater never changes project policy or ownership to clear a refusal.

The updater also handles a registered project that the previous
service could not admit after rollback. During a qualified Core transition,
the installer checks the previous service using its exact Observer/Core versions
and verifies the registered root and its enrollment error. A known quality or
temporary refusal records an unready baseline; an unknown root, registry conflict,
identity change or integrity failure still stops recovery. The target must qualify
every project's retrieval readiness under the existing publication grants before
installation succeeds. A search-quality warning alone does
not reject an update; incompatible, unsafe or stale context still does.
Qualification errors retain the failing project's opaque hash and bounded gate
diagnostics, including failures before service startup.

Updates record the previous readiness baseline for every
kit update, including updates that keep the same Core. A source-limit refusal is
an explicit degraded baseline, not a transient failure or target readiness.
Cold admission can verify retained partial context without a saved limit
observation; the first controller check preserves its admitted partial result.

An older failed update may retain a runtime journal without that baseline. The
following option is only for that explicitly reviewed recovery case.
After reviewing the verified previous kit and its known content refusals, run
from a new complete extracted kit:

```sh
./bin/shin-ce update --prefix /path/to/installed-kit --accept-degraded-rollback
```

This option only finishes recovery of the previous payload. It is limited to a
legacy same-Core rollback with no recorded baseline or bootstrap migration. It
does not start a new update, mark degraded projects ready, or accept unknown,
identity, integrity or permission failures. A confirmed recovery reports
`installation-update-rolled-back`; verify `recovery_pending=false` and review
`last_rollback.pending_projects` before explicitly starting another update.

Monitor's project list shows the visible range and total in EN/RU. A short final
page does not mean the remaining projects were removed.

Startup keeps a 60-second no-progress budget while admitting registered projects.
The first admission or readiness outcome of each selected project renews that
budget, up to a five-minute total startup bound. Repeated status replies, changed
snapshots and unrelated projects do not extend it. All project readiness and
rollback checks still apply; an expired wait reports `service-enrollment-timeout`.

Updating with the exact installed archive is a no-op. A changed archive must have
a newer version; downgrades and altered same-version kits are refused. The updater
applies the version and project-qualification rules described in
[Core transitions and index rebuilds](#core-transitions-and-index-rebuilds).
The extracted-kit command does not download releases or change shell, client or
model-usage configuration. Its bounded generator-pin transition is described below;
other project settings and profile policy are preserved.

### Preview publication and CI

Only a CI-qualified published Release is offered to users. Maintainers prepare
`docs/releases/<tag>.md`, commit the matching Observer version, and push its
reviewed `vX.Y.Z-dev.N` tag. Each platform waits for its own Core/Observer
source jobs and exact package qualification. Linux, Windows and macOS publish
independently; a queued macOS ARM runner does not hold ready Windows or Linux
downloads. The Linux terminal archive targets Ubuntu 24.04 x86_64, with its
declared system libraries; it does not include a desktop Monitor.
`tools/publish_preview.py` independently checks the source commit, workflow,
attempt, platform job results and artifact provenance, then downloads that
platform's exact CI archive and checksums. Local binaries cannot be substituted.
A pending or failed platform has no published offer. Already qualified platforms
stay available while another platform waits.

Publishers share a per-tag [queued concurrency group](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
with `queue: max`; adding one platform cannot cancel another pending publisher.
Each `shin-preview-PLATFORM.json` binds the platform assets to their source, run,
attempt and hashes. Appending a platform revalidates existing assets against
their original CI artifacts, including a previous successful attempt. Unknown,
changed or incomplete proven assets are preserved for review, never replaced.
Retain those CI artifacts while another platform is pending (currently 14 days).
The macOS kit ZIP is uploaded after its updater, installer and checksums so an
already public Windows/Linux release cannot expose an incomplete macOS offer.

"Do not wait for CI" means the maintainer can finish after pushing the tag. The
workflow completes publication when qualification succeeds. It does not permit
manual publication of a locally tested kit while CI is pending. Monitor reads
published Releases; its checks do not expose draft releases or Actions artifacts.

GitHub may briefly report a completed native job before its step metadata is
complete. The publisher refetches missing package proof for up to 27 seconds;
failed proof still refuses immediately, and no download or release mutation
precedes qualification. If only the publication job failed, maintainers can run
**Recover qualified preview publication** from `main` with the existing tag,
source qualification run ID, exact attempt and platform. It checks out that
source and publishes its original CI archive after the same platform gates.
Rerunning all jobs is required if a rerun attempt lacks either of that platform's
source jobs. Recovery cannot qualify failed native tests, change a tag, rebuild
binaries or replace release assets. The current publisher and platform receipts
must belong to this independently qualified workflow; an older all-platform
candidate needs a new reviewed tag rather than retroactive publication policy.

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

From a macOS source checkout, use a C++20 compiler and standard library with
`std::jthread` and `std::stop_token`, CMake 3.24+, SQLite with FTS5
3.38+ and OpenSSL Crypto. Command Line Tools for Xcode 26 or newer, or a current
full Xcode installation, provides the required Apple standard library. Full Xcode
and an Apple Developer Program membership are not required for local compilation.
Python 3.11+, Bash, and jq are needed for the checks.
CMake downloads pinned dependencies with checked hashes, including FTXUI 7.0.3
(MIT) for the terminal monitor. FTXUI is linked statically; users need no
additional terminal UI library. Its license ships under
`share/shin-observer/licenses/ftxui/`.

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

Exact-archive qualification tests the current candidate against the published
previous-generator kit pinned in
`.github/fixtures/macos-previous-generator.json` (currently dev.66 / Core dev.11,
generator v23). To repeat that check with a complete current kit archive:

```sh
python3 -B observer/installer/previous_generator_test.py \
  --cmake "$(command -v cmake)" \
  --candidate-archive /absolute/path/to/shin-local-VERSION-macos-arm64.zip \
  --proof /tmp/shin-previous-generator-proof
```

This downloads and verifies the pinned old archive, then runs forward update,
interrupted rollback/resume and refusal of unapproved profile-policy or marker
edits in disposable installations. Canonical and bootstrap projects use real
adoption markers pinned to the old generator. The checks retain registry,
identity, grants, source bytes and SQLite history. No live installation or managed
index is repaired. GitHub offer discovery and the interactive Monitor handoff
remain separate checks. The older `previous_update_test.py` is a historical
Core dev.3 → dev.4 regression fixture, not a current-kit qualification command.


## Core transitions and index rebuilds

The native kit admits forward Core transitions within the same major
version, subject to installation and project qualification. This replaces the
historical list of individual version pairs. Downgrades are not normal updates;
rollback uses the verified previous payload recorded by the installation journal.

The current generator is `shin-generator-v24-objective-cpp`. Older format identifiers are recognized
only to rebuild approved source inputs, not to read or convert old retrieval
data. Canonical publications are refreshed by the ordinary Observer under
existing grants. Private-bootstrap bindings are updated through native
initialization without new enrollment or changed identity/profile policy.
All projects must qualify before success.

If an already approved project marker pins the previous generator, the installer
journals the original and replacement bytes, changes only that generator value
and uses native `marker-accept` before rebuilding. Pending owner edits or an
unapproved profile remain refusals. Rollback restores the original marker bytes
and approval through the previous CLI. This is a bounded installation operation;
ordinary agents and MCP reads cannot approve marker changes.

Interrupted recovery resumes from the same archive. Failed activation restores
the previous kit and rebuilds its derived indexes when necessary, including the
owned bootstrap retirement described above. Source files, project settings,
canonical grants and history remain preserved throughout activation and recovery.

### Release offers in Monitor

The macOS Monitor checks the public GitHub Releases feed shortly after launch and
then every six hours. No account, token, project names or source content are sent.
GitHub sees a normal HTTPS request and its network metadata. Disable automatic
checks or choose **Check for updates…** in the tray menu. Failed checks do not
interrupt offline retrieval. Manual checks fetch the current offer even after a
recent check; concurrent checks are coalesced.

The launcher and independent Updater use the same 1 MiB release-feed limit for
downloading and parsing metadata. Local state
keeps its separate 256 KiB limit. Duplicate JSON keys, excessive nesting and
invalid release assets are still rejected.

Monitor uses one persistent **Shin updates** window for manual
checks, the offer, launch failures and saved results. Opening it closes the tray
popover and brings that window forward. Release notes open the browser while
preserving the review; copying diagnostics preserves the result and its actions.
Background replies update the window without stealing focus from another
application. A failed explicit installation returns its
still-open result window to the foreground; a closed or minimized window stays
dismissed. A manual request during a background check
shows that check's result instead of discarding the request. After selecting
**Download and install**, the window shows the helper handoff before Monitor exits.

Current development source gives these update windows the same dark Sand
surfaces and readable, compact actions as the dashboard. It retains the independent Updater handoff, saved results and recovery
checks described below.

A preview installation receives newer complete preview or stable kits; a stable
installation receives stable releases only. Drafts, incomplete uploads, releases
without a matching macOS ARM64 ZIP/checksum pair and invalid asset URLs are not
offered. The badge and panel footer lead to a review showing the version and
release notes. Nothing is installed until **Download and install** is selected.

The helper rechecks the exact reviewed offer, waits for Monitor to close, downloads
the archive over HTTPS and verifies its byte count, GitHub SHA-256 digest, checksum
file, archive paths and complete native manifest before invoking the new kit's
existing updater. That updater retains installation ownership, compatibility,
project qualification, data preservation and rollback checks. The helper records
an outcome and reopens Monitor. Clients that have loaded the update-aware MCP
bridge resume compatible connections; older adapters need one reconnection.
The helper's own progress window remains visible while Monitor is closed. It
opens before the reviewed release is rechecked. It shows the complete list of
steps, the current step and its elapsed time, and marks observed completed steps.
Download and staging percentages use real byte counts. Project compatibility and
readiness checks show completed/total project counts. Service shutdown, kit swap,
startup, final confirmation and rollback each have their own status. Work without
a measurable total uses an indeterminate indicator, not an estimated overall
percentage. The window uses Monitor's selected EN/RU language and does not offer
cancellation once the native operation has started. CLI updates show the same
stages on a terminal, in the language selected by LC_ALL/LC_MESSAGES/LANG. stdout
retains the JSON result; `--progress` enables readable stderr output in redirected
runs. The helper filters child progress separately from the native receipt, and
only the independent installed-kit readback confirms completion. The downloaded
independent Updater supplies the progress window; the bootstrap's initial handoff
is described below.
Checksums protect download integrity; this preview is not Developer ID signed or
notarized and does not provide an independent signature over a compromised release.

For terminal use:

```sh
shin-ce update-check
shin-ce update-install --revision HASH_FROM_CHECK
shin-ce update-status
```

Current development source separates installation from project readiness. A verified
kit and healthy service can complete an update while individual projects refresh
or retain a search/validation diagnostic. Those projects keep their normal
freshness, policy and quality gates; installation does not approve new profiles,
raise limits or make withheld context readable.

The saved result distinguishes a retryable refusal before replacement, an
interrupted native transaction requiring recovery, and a confirmed installation.
Monitor and terminal stats show the original failure and offer **Retry update** or
**Continue update** when independent installation readback permits that action.
These actions use the exact saved result revision; another operation or a changed
installation invalidates the review. The CLI entry is:

```sh
shin-ce update-status
shin-ce update-resume --revision HASH_FROM_STATUS
```

A still-valid saved candidate is verified again before reuse. Recovery belongs to
the recorded target, never an unrelated newer release. Missing or replaced state,
changed ownership and unverified payloads remain refusals. **Last update result…**
and **Copy diagnostic report** preserve the original failure for diagnosis;
acknowledgement is separate from repair and never confirms an unfinished native
transaction.

### Independent Updater (dev.49 and later)

Starting with dev.49, Shin separates the installed bootstrap from the updater
shipped with each release. The release adds `shin-updater-v1-macos-arm64.zip` and
its SHA-256 file.
The bootstrap understands only the stable [updater envelope and protocol](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/machine/updater-bootstrap-v1.json),
not the application's manifest, Core generator or native recovery journal.

A manual/background check selects a complete updater offer without downloading
executable code. After installation is confirmed, the bootstrap rechecks that
exact offer, verifies the updater archive, its file inventory and protocol, then
hands off once. The fresh updater checks the installed kit and downloads/verifies
the matching application release before invoking its native update. A malformed
application manifest cannot prevent delivery of a newer updater; the fresh updater
still refuses unsafe ownership, unsupported state or missing authority.

Monitor's PID handoff is preserved with `exec`. The downloaded updater supplies
its own progress window, so its update UI and recovery logic travel together.
CLI uses that updater's terminal output; TUI observes its progress and native
receipt. The initial Updater download stays in Monitor's opening-progress state;
TUI shows its actual received bytes as preparation progress.

Saved outcomes retain the verified updater when recovery/review is unresolved.
Status and acknowledgement run it without network access. After confirmed success
or verified acknowledgement, the owned temporary updater is removed; the complete
installed kit includes an updater for subsequent offline result inspection.
Read-only protocol/check probes never delete a downloaded updater awaiting handoff.

If the installed launcher itself cannot run, download the versioned `install.sh`
again. It fetches the fresh complete kit and starts that kit's `shin-updater
install-local`, which validates it and delegates to the native installer. Existing
identity, settings, projects and history remain under native ownership checks.
An existing installation retains its recorded data and command directories when
the caller supplies only its prefix; defaults apply only to a first installation.
The independent entry also supports resuming an interrupted native update.

Older launchers that cannot complete an update can use the versioned independent
installer above. Release hashes provide download integrity under the
existing GitHub HTTPS trust boundary; this preview has no independent publisher
signature and remains without Developer ID/notarization.
