# Local macOS kit

The kit includes Core, Observer, the `shin-ce` CLI, `shin-mcp`, the usage collector,
Shin Monitor, profiles, contracts, and dependency notices. Local retrieval needs
no account or AI model service.
The current development preview targets macOS 15+ on Apple Silicon. It uses an
ad-hoc signature, without Developer ID or notarization; first launch on another
Mac may require manual approval. This is not a stable release.

This guide covers the macOS kit. Native Windows source builds and the separate,
Windows ZIP/MSI packaging route are described in [Windows source status](https://github.com/toseek-samurai/shin-context-engine/blob/main/docs/windows-desktop.md).

## Download a preview release

Use the assets attached to the [versioned preview release](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.62).
The release contains the kit and independent Updater ZIPs, their SHA-256 files,
and `install.sh`.
Public release downloads do not require a GitHub account.

For one-command installation or update:

```sh
curl -fsSL https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.62/install.sh | sh
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

In dev.53 source, a project using the default bootstrap profile keeps serving
verified context when local Markdown links are broken. Projects and its details
show a documentation warning, which clears after the corrected sources qualify.
Index integrity, source freshness, secrets and resource checks remain required.
Preview dev.57 separates retrieval availability from declared search quality
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

Monitor supports English and Russian. Use the language selector at the top of
the panel; it applies immediately, including open native windows and menus. System
uses the operating system language, with English as fallback. Translations are
compiled into the kit and need no downloads.

Click the Shin icon in the menu bar. The update indicator
stays inside the same square icon. macOS can still hide menu-bar extras when
space is tight; it provides no public always-visible priority for an app's icon.
Preview dev.62 runs **Shin Monitor.app** as a tray application: opening it
shows the popup, and closing the popup leaves Monitor running. The dashboard
stays attached to its popover; it does not detach into a standalone window or
add a Dock icon. If the menu bar is crowded, hold Command and drag the Shin
icon to a visible position. An explicit Quit exits Monitor.

In preview dev.62, the installed Monitor registers itself with macOS Login Items
on its first launch. Use **Launch Monitor at login** in its app or
status-item menu to disable or re-enable this. The checkmark reads the actual
macOS setting; external opt-out is preserved. If approval is needed, the menu
opens Login Items in System Settings. Development and portable builds do not
register themselves. Login launches stay in the background; an explicit open
shows the dashboard. Uninstall removes the registration owned by that bundle.
The background service has a separate login-startup setting.

Add more projects with the folder picker or
`shin-ce project-add --root /path/to/another/project`.

For the Sand terminal dashboard with line charts, run `shin-ce stats`.
Use Tab or 1–6 for Overview, Projects, Timing, Storage, Models and Sessions,
including MCP activity and explicit host context markers when available.
Unreleased source adds **Updates** as view 7 (`u`): `r` checks releases, `i` reviews
installation, `y` confirms and `n` or Esc cancels. Its progress and saved results
use the same native updater as Monitor. Automatic release download is macOS-only.
Enter opens project details,
`?` shows keyboard help and `q` exits. The dashboard observes the existing
service. Model counters are collected automatically
for enrolled projects and shown by default in both Monitor and the TUI. The first
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
`generic-source` profile, which explicitly allows 15,000 source files and 1 MiB
per file. The language-specific bootstrap profiles allow 10,000 files and 1 MiB
per file. These are extraction budgets, not the amount of text sent to an agent
or a guarantee about peak indexing memory.

In current development source, Core defaults to 10,000 files and 1 MiB per file
when a profile omits those fields, matching the language-specific templates.
The bundled new-project template declares its budgets explicitly. An existing
permanent project keeps its explicit reviewed limits. `profile-policy` reports
the effective values and whether each came from the profile or Core. Check that
output for the installed kit; source defaults take effect with a new release.

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

In current development source, `profile-relocate` moves an enrolled, active,
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

In dev.13, Monitor opens
**Set up Shin** on a fresh installation. Choose the first folder, select agents
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

Monitor displays its installed version beneath the panel title, including while
offline. An available newer version appears separately in the update offer.

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
Restart Monitor after completion. Restart persistent MCP connections in your coding
agent as well: already-running adapters retain the old version and cannot query
the new service until relaunched. Client configuration paths stay the same.
The dev.45 kit diagnoses this as `service-version-mismatch`, includes client
and service versions and asks for client reconnection. Successful responses still
require matching versions; the current service can keep serving other clients.

An interrupted operation is resumed by repeating the same command from the same
extracted kit. Failed activation restores the previous payload and service; the
error reports whether rollback completed or further recovery is required. One
previous kit is retained outside the active payload. A subsequent update or
uninstall removes it only after verifying its ownership and contents. Unexpected
files or corruption are preserved for review, never silently deleted.

The dev.45 kit uses Core dev.7 and the Unicode v23 index format. It
rebuilds previous derived indexes under their existing project authority; no
old retrieval reader or term conversion is retained. During rollback to an
older kit, a private bootstrap index that the old CLI cannot read is atomically
retired through the owned temporary-publication lifecycle before the old CLI
builds it again. The installation journal retains pending recovery throughout.
Bindings, profiles, sources, grants, identity and history are not deleted.

Starting with the dev.20 source candidate, failed activation retains bounded
enrollment error codes and opaque project hashes in `error.diagnostic` and
`shin-ce installation` (`last_update_diagnostic`). Rollback checks the previous
service independently if its startup acknowledgement fails. A restored service
may have temporarily blocked projects: `last_rollback.service_restored` then stays
separate from `all_projects_ready` and `pending_projects`. These fields describe
the rollback observation, not current readiness; use `service-status` and project
`status` for the current state. Identity, integrity and registry failures still
leave recovery incomplete. The updater never changes project policy or ownership to clear a refusal.

The dev.33 source candidate also handles a registered project that the previous
service could not admit after rollback. During a qualified Core patch transition,
the installer checks the previous service using its exact Observer/Core versions
and verifies the registered root and its enrollment error. A known quality or
temporary refusal records an unready baseline; an unknown root, registry conflict,
identity change or integrity failure still stops recovery. The target must qualify
every project's retrieval readiness under the existing publication grants before
installation succeeds. Since preview dev.57, a search-quality warning alone does
not reject an update; incompatible, unsafe or stale context still does.
Qualification errors retain the failing project's opaque hash and bounded gate
diagnostics, including failures before service startup.

Preview dev.41 and newer record the previous readiness baseline for every
kit update, including updates that keep the same Core. A source-limit refusal is
an explicit degraded baseline, not a transient failure or target readiness.
Cold admission can verify retained partial context without a saved limit
observation; the first controller check preserves its admitted partial result.

An older failed update may retain a runtime journal without that baseline. The
following recovery option is included in preview dev.41 and newer; it is not in dev.39.
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
It does not download releases or change shell,
client, project or model-usage configuration.

### Preview publication and CI

Only a CI-qualified published Release is offered to users. Maintainers prepare
`docs/releases/<tag>.md`, commit the matching Observer version, and push its
reviewed `vX.Y.Z-dev.N` tag. The tag workflow waits for all
four Linux/macOS Core and Observer jobs and both Windows jobs, including exact
macOS and Windows package qualification, before publishing either platform.
`tools/publish_preview.py` independently checks the source commit, workflow,
attempt, job results and artifact provenance, then downloads that run's exact
macOS kit/updater and Windows ZIP/MSI with their checksums. Local binaries cannot be substituted. A pending
or failed qualification leaves no published offer; a mismatched existing release
is preserved for review rather than overwritten.

"Do not wait for CI" means the maintainer can finish after pushing the tag. The
workflow completes publication when qualification succeeds. It does not permit
manual publication of a locally tested kit while CI is pending. Monitor reads
published Releases; its checks do not expose draft releases or Actions artifacts.

GitHub may briefly report a completed native job before its step metadata is
complete. The publisher refetches missing package proof for up to 27 seconds;
failed proof still refuses immediately, and no download or release mutation
precedes qualification. If only the publication job failed, maintainers can run
**Recover qualified preview publication** from `main` with the existing tag,
source qualification run ID and exact attempt. It checks out that source and
publishes its original CI archive after the same gates. It cannot recover failed
native qualification, change a tag, rebuild binaries or replace release assets.

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
3.38+ and OpenSSL Crypto.
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

To reproduce recovery against real previous executables in a disposable home,
extract the official dev.28 kit (Core dev.3) and the candidate kit (Core dev.4), then
run:

```sh
python3 -B observer/installer/previous_update_test.py \
  --previous-kit /tmp/shin-previous-kit \
  --candidate-kit /tmp/shin-candidate-kit \
  --report /tmp/shin-previous-update-proof.json
```

The fixture enrolls one canonical project and one bootstrap project, forces a
native activation rollback, then retries while the old service is running with a
rejected project. It checks target readiness, service restoration, registry,
identity, grants and existing history. No live installation or managed index is
repaired. For the regression baseline, use the unmodified dev.32 kit as candidate
and add `--expected-error installation-recovery-command`. This exercises the
native installation child used by automatic updates; HTTPS delivery and the
Monitor offer are separate checks.


## Core transitions and index rebuilds

The dev.45 kit admits forward Core transitions within the same major
version, subject to installation and project qualification. This replaces the
historical list of individual version pairs. Downgrades are not normal updates;
rollback uses the verified previous payload recorded by the installation journal.

The qualified transition from the dev.42 kit upgrades Core dev.6 to dev.7 and
rebuilds v22 indexes into Unicode v23. Older format identifiers are recognized
only to rebuild approved source inputs, not to read or convert old retrieval
data. Canonical publications are refreshed by the ordinary Observer under
existing grants. Private-bootstrap bindings are updated through native
initialization without new enrollment or changed identity/profile. All projects
must qualify before success.

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

Preview dev.57 uses the same 1 MiB release-feed limit for downloading and
parsing metadata in both the launcher and the independent Updater. Local state
keeps its separate 256 KiB limit. Duplicate JSON keys, excessive nesting and
invalid release assets are still rejected.

Monitor uses one persistent **Shin updates** window for manual
checks, the offer, launch failures and saved results. Opening it closes the tray
popover and brings that window forward. Release notes open the browser while
preserving the review; copying diagnostics preserves the result and its actions.
Async replies update the window without stealing focus from another application
or reopening a window the user closed. A manual request during a background check
shows that check's result instead of discarding the request. After selecting
**Download and install**, the window shows the helper handoff before Monitor exits.

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
an outcome and reopens Monitor. Persistent MCP clients still need to reconnect.
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
only the independent installed-kit readback confirms completion. This window is
supplied by the installed helper, so the change takes effect for updates started from the new kit.
Checksums protect download integrity; this preview is not Developer ID signed or
notarized and does not provide an independent signature over a compromised release.

For terminal use:

```sh
shin-ce update-check
shin-ce update-install --revision HASH_FROM_CHECK
shin-ce update-status
```

An update failure or missing acknowledgement is retained for review, including the
candidate path when installation may have started. Monitor offers to copy the
result; it does not silently retry. `update-status` reads the durable outcome. Use
the native `installation` command and the saved candidate's `update` command to
review/reconcile its journal before trying another installation. Project refresh
under heavy machine load may still delay or refuse update activation.

Release update checks are included in dev.19 and newer kits. Existing dev.16/dev.17
binaries acquire them only after an explicitly selected new kit is installed.

**Last update result…** remains available in the tray menu. After resolving a
native recovery, **Acknowledge verified result** verifies the installed payload
and completed journal before clearing the review gate and cleaning its identified
private staging folder. A stale review or unfinished native operation is refused.

### Independent Updater (dev.49 and later)

Starting with dev.49, Shin separates the installed bootstrap from the updater
shipped with each release. The release adds `shin-updater-v1-macos-arm64.zip` and
its SHA-256 file.
The bootstrap understands only the stable [updater envelope and protocol](machine/updater-bootstrap-v1.json),
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

This source change first takes effect after installing a kit that contains it.
Older clients can acquire that kit through their existing full-kit update path or
the independent installer. Release hashes provide download integrity under the
existing GitHub HTTPS trust boundary; this preview has no independent publisher
signature and remains without Developer ID/notarization.
