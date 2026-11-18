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

Use the assets attached to the [versioned preview release](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.124).
The release contains the kit and independent Updater ZIPs, their SHA-256 files,
and `install.sh`.
Public release downloads do not require a GitHub account.

For one-command installation or update:

```sh
curl -fsSL https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.124/install.sh | sh
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

A wiki with a root `Home.md` can connect without a README. Shin indexes the
Markdown pages next to `Home.md`, including `_Sidebar.md` and `_Footer.md`, and
tracks additions, edits and removals. A wiki link whose destination is
`Rotation#handover` resolves to the sibling `Rotation.md`. Hidden files and
symlinks are excluded.

Broken local Markdown links appear as documentation warnings while verified
context stays available. The warning clears after the corrected sources qualify.
Secret quarantine, source freshness, index integrity, agent grants and resource
limits still apply. Project details show omissions and the reason for a refusal.

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

The current native preview uses dark Sand with **Overview**, **Projects**,
**History** and **Settings**. Short pages fit their contents; longer views scroll
within the panel. Earlier desktop previews use **Activity** as their third section.
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
Monitor keeps the setup window visible during preparation, showing the
current stage and elapsed time. Failures remain inline with a retry action;
successful preparation leads to agent selection in the same window.

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

### Project text and exclusions

The development source registers the project folder and discovers eligible
text recursively, including extensionless files and unfamiliar suffixes. Known
formats can supply structured evidence. Other text supplies bounded lexical
excerpts with source locations. Adding a README, defining a source layout or
writing a project profile, adoption marker or evaluation suite is unnecessary.

Shin reads `.gitignore` in the chosen folder and its subdirectories. Patterns
match paths case-sensitively, including tracked files that match a rule. A nested
`.gitignore` refines rules for its subtree. Git's index, global excludes and
`.git/info/exclude` are outside this selection. The same rules work in a folder
without a Git repository or Git executable. [Git pattern reference](https://git-scm.com/docs/gitignore#_pattern_format).

An optional UTF-8 `.shinignore` at the project root takes precedence. Use it
to exclude another path or start a rule with `!` to bring back eligible text,
even inside a Git-ignored directory. The last matching Shin rule wins.
Registration creates no file. For example, if `.gitignore` contains
`/local-notes/`, this `.shinignore` keeps just the handbook searchable and
also hides scratch files throughout the project.

```text
!/local-notes/handbook.md
*.scratch
```

Patterns use `/` between components. A leading slash anchors a pattern to the
rules file's directory, a bare name matches at any depth, and a trailing slash
selects directories. Use `*`, `?` or bracket ranges within a component and
`**/` to match any number of directories. Blank lines and lines starting with
`#` are ignored. The root `.shinignore` accepts up to 256 rules of 1,024 bytes
each in 32 KiB. Each project `.gitignore` can be up to 1 MiB. A symlinked
`.shinignore` is refused. A symlinked `.gitignore` supplies no rules.

The service observes changes to exclusions and project files. It verifies the
new scope before serving affected evidence. The kit skips Git metadata,
dependency trees such as `node_modules`, generated build output, symlinks
and known credential files. Configuration such as `.github/workflows` can
provide eligible text. File safety checks apply with or without a rules file.
Use the reported coverage and omissions when interpreting an empty result.

The managed reader's `project_files` identifies the exclusion revision and
reports excluded entries, oversized files and binary content. An excluded
directory counts once, regardless of how many children it contains. Secret
quarantine is reported separately in `source_coverage`.

For projects with more than 8,192 eligible files or too much text for the
full fragment index, Shin first ranks
files using terms from each eligible source. It then reads matching files
against their snapshot hashes and returns exact source ranges. The
`file_coverage` field reports the number of files examined and whether more
matched the question. A narrower question or an exact file path can help
when a broad query fills that window.

The bundled policy allows 30,000 eligible files and 2 MiB per file.
`source_scope.max_files` and `source_scope.max_file_bytes` report the active
limits. Aggregate indexing, parser work and the response to each query have
their own bounds. Text discovery accepts UTF-8 text and rejects binary control bytes. Files
without a supported parser may still provide text evidence. Binary,
unsafe or unreadable input cannot supply source excerpts. A reported refusal
or quarantine includes the available diagnostic and preserves the limits.

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
checks project authority and derived state under the new engine, stages complete
files, stops the owned service and atomically exchanges the payload directories.
It restores the previous service/autostart preference and confirms the installed
kit and service. Projects still preparing or retaining a permitted diagnostic
are reported separately as `projects_pending`; their own retrieval gates remain enforced.
Project bindings, indexes, identity and history remain in their existing data home.
Terminal updates show each stage and elapsed time on stderr, with real byte counts
for copying and completed/total project counts for qualification. stdout retains
the JSON receipt. Redirected commands stay quiet by default; add `--progress` to
include readable stages in a log. No desktop window opens for a terminal command.
Restart Monitor after completion. The current native kit includes an update-aware
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

The current Core uses the Objective-C++ v24 index format. A new native kit
rebuilds derived indexes from approved project sources under their existing
authority, even when the format stays the same. During rollback to an
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
identity change or integrity failure still stops recovery. The target preserves publication authority and prepares each project's new
generation independently. A preparing or blocked project does not disappear
and cannot provide unqualified context.
Qualification errors retain the failing project's opaque hash and bounded gate
diagnostics, including failures before service startup.

Updates record the previous readiness baseline for every
kit update, including updates that keep the same Core. A source-limit refusal is
an explicit degraded baseline, not a transient failure or target readiness.
Cold admission can verify retained partial context without a saved limit
observation; the first controller check preserves its admitted partial result.

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
The extracted-kit command uses the local archive and keeps shell, client and
model-usage settings. The project view shows preparation progress.

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
`std::jthread` and `std::stop_token`, CMake 3.24+ and OpenSSL Crypto.
CMake builds checksum-pinned SQLite 3.54.1 statically with FTS5; a separate system
SQLite development package is not needed. Command Line Tools for Xcode 26 or newer, or a current
full Xcode installation, provides the required Apple standard library. Full Xcode
and an Apple Developer Program membership are not required for local compilation.
Python 3.11+, Bash, and jq are needed for the checks.
For a macOS 15-compatible archive, OpenSSL must also support macOS 15. A newer
Homebrew library can require the build host's newer OS; setting the application's
deployment target does not retarget that library. Select a matching development
prefix with `OPENSSL_ROOT_DIR` before building the kit.
CMake downloads pinned dependencies with checked hashes, including FTXUI 7.0.3
(MIT) for the terminal monitor. FTXUI is linked statically; users need no
additional terminal UI library. Its license ships under
`share/shin-observer/licenses/ftxui/`.
Development source also pins libuv 1.53.0 for the event-driven local coordinator.
It is linked statically; its license and supplemental notices ship under
`share/shin-observer/licenses/libuv/`. No separate runtime installation is needed.

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

This downloads and verifies the pinned old archive and exercises a forward
update and interrupted rollback/resume in disposable installations. Include
previously enrolled folders with legacy profile and marker files. Compare the
original source, marker and external index bytes across the transition, and
verify enrollment, identity, grants and SQLite history. GitHub offer discovery and the interactive Monitor handoff
remain separate checks. The older `previous_update_test.py` is a historical
Core dev.3 → dev.4 regression fixture, not a current-kit qualification command.


## Core transitions and index rebuilds

Every new kit rebuilds managed indexes, reader caches and qualification state
from the current project sources under its own rules, even when those sources
and the generator format have not changed. Previous derived data is disposable;
there is no historical index reader, format conversion or version-pair allowlist.
Downgrades are not normal updates. Transactional rollback uses the verified
previous payload retained by the installation journal.

The current generator is `shin-generator-v24-objective-cpp`. A fresh kit
keeps the enrolled project folders, agent settings, grants and history. It
rebuilds managed evidence from current files with its own discovery rules and
each project's `.gitignore` files and optional root `.shinignore`. Existing client selections continue
to govern access after the new generation qualifies. A previous project profile
or generator-pinned marker can remain in the source tree. Its contents are
unneeded for managed registration and rebuilding, even if a profile changed or
is missing. The kit leaves those files and existing external indexes intact.

A confirmed kit/service update may report projects as preparing or blocked.
They stay visible in Monitor and provide no context until the new generation
passes the required source, integrity, safety and resource checks. Installation does not wait
for every active worktree to settle and does not treat previous indexes as
readiness proof for the new kit.

A project already exceeding its declared source limits may therefore remain
blocked after a kit change. Previously retained partial context does not bypass
the new generation's safety checks. Inspect the reported limits and omissions.
An owner may choose exclusions for material outside the intended search scope.

Monitor and `shin-ce stats` distinguish automatic recovery from an action that
needs your help. Technical preparation failures retry in the background. The
project view offers the relevant settings/search review, project folder,
storage review or diagnostic report. These actions preserve approval and safety
checks; opening a report or a folder does not repair or approve a project.

Interrupted recovery resumes from the same archive. Failed activation restores
the previous kit and rebuilds its derived indexes when necessary, including the
owned index retirement described above. Source files, project settings,
grants and history are preserved throughout activation and recovery.

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

The current preview gives these update windows the same dark Sand
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

The current native preview separates installation from project readiness. A verified
kit and healthy service can complete an update while individual projects refresh
or retain a search/validation diagnostic. Those projects keep their normal
freshness, authority, safety and resource checks. The verified generation and
current source bindings determine what context can be read.

The saved result distinguishes a retryable refusal before replacement, an
interrupted native transaction requiring recovery, and a confirmed installation.
Monitor and terminal stats show the original failure and offer **Retry update** or
**Continue update** when independent installation readback permits that action.
These actions use the saved result's revision. Starting with dev.111, the Updater
derives that revision from the same receipt bytes it checks against the verified
installation. A different receipt or another saved attempt requires a fresh review.
The CLI entry is:

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
