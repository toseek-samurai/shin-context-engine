# Shin in the terminal

The installed CLI uses the same local service and data home as Monitor. From an
enrolled project directory:

```sh
shin-ce status --text
shin-ce service-status --text
shin-ce query "Where are requests validated?"
```

## Interactive terminal monitor

On macOS and Linux, open a full-screen, read-only dashboard:

```sh
shin-ce stats
```

For a portable kit, pass `--home /absolute/path/to/private-data`. An installed
kit uses its recorded home. The monitor reconnects automatically when the
service returns, and labels retained observations as stale while disconnected.
It does not start a service or enroll projects. It requires an interactive
terminal with at least 60 columns and 12 rows; text/JSON commands work in pipes.

| Key | Action |
| --- | --- |
| `1`–`5`, Tab, Left/Right | Overview, Projects, Timing, Storage, Models |
| Up/Down or `j`/`k` | Select a project; scroll other views |
| Enter, Esc | Open project details; return |
| PageUp/PageDown | Scroll a page |
| `t`, `f` | Toggle recent/retained timings; filter query traffic |
| `p` | Next usage project in Models |
| `r` | Refresh observations; explicitly rescan storage in Storage |
| `?`, `q` | Keyboard help; quit |
| Ctrl-L | Redraw the screen |

Service observations refresh every two seconds. Storage is inspected when you
open its tab or press `r`, not continuously. Usage is read every 30 seconds or
on refresh, using the same normalized file and parser as Monitor:

```sh
shin-ce stats --usage-file /absolute/path/to/usage/state.json
```

The usage file must be a private collector snapshot, not a configuration file.
Without it, Models shows setup guidance. No client-session scanning or usage
collection is performed by the TUI. Partial, stale, and missing measurements
remain explicit. Project management and cleanup use the existing CLI commands;
the first TUI version is for observation.

`status` describes the current project. `service-status` lists all projects in
the selected service, their search and observer states, recent memory usage,
active queries, and request counters for the current run. Omit `--text` for JSON.

Text status distinguishes missing measurements from zero and preparing or
standby readers from blocked projects. A standby project is still observed; its
search reader warms when needed.

## Monitor and CLI

| In Monitor | In the CLI today |
| --- | --- |
| Project readiness and partial coverage | `shin-ce status --text` |
| All projects, service health, recent RAM and request counts | `shin-ce service-status --text` |
| Detailed request telemetry and retained history summaries | TUI Timing with recent/history and traffic filters; `shin-ce service-status` JSON |
| Add a project from a folder | `shin-ce project-add --root /absolute/path/to/project` |
| Storage inventory and cleanup plan | TUI Storage or `shin-ce storage-status` JSON; cleanup remains a separate command requiring the reported revision |
| Profile changes requiring review | `shin-ce profile-review`; acceptance is a separate explicit action |
| Model usage | TUI Models uses automatic enrolled-project collection in dev.12; `--usage-file` remains an explicit override |

The TUI shares Monitor's project-state and request-timing calculations. It uses
native service status for observations and the existing read-only storage
inventory command. JSON remains available for automation.

Monitor and stats support English and Russian. Monitor's language selector uses
System, English or Русский and applies on restart. Stats follows `LC_ALL`, then
`LC_MESSAGES`, then `LANG`; unsupported languages fall back to English. Override
with `shin-ce stats --language en` or `--language ru`, or press `l` to switch for
the current terminal session. Russian text requires a UTF-8 terminal locale.
This choice affects display text only: command
names, JSON fields, error codes and stored data keep their existing format.

## Inspect evidence

Queries return structured evidence and a snapshot identifier. Use that returned
identifier with project-relative paths for the following reads:

```sh
shin-ce inspect src/example.cpp --snapshot SNAPSHOT_ID
shin-ce read src/example.cpp --snapshot SNAPSHOT_ID
```

Pass `--root /absolute/path/to/project` when working from another directory. A
portable extraction also needs the same explicit `--home` used at initialization.
See [installation](local-kit.md) and [MCP setup](native-client-setup.md).

Shin's usage collector observes supported local client usage records. It is not
required for indexing or retrieval. Adding a project enables automatic collection
for that project. Counters depend on readable client history; missing history is
reported as incomplete. See [the local kit guide](local-kit.md)
for configuration details.

### Interpreting service activity

Timing shows separate current-service-run counters for query, inspect, read and
project status calls. Project details expose the same breakdown (Monitor tooltip,
TUI details and service-status JSON). These counters include completed or refused
observed calls; they reset with the service, and a project's counters reset when it
is detached. They are separate from retained request history and query timings.
Successful status checks do not change the historical retrieval count.

Diagnostic calls are explicitly tagged. Other calls remain unclassified: they
may include agent traffic, CLI checks or installation probes. `reported_sessions`
counts distinct valid opaque IDs actually supplied by callers (bounded to 256 for
the run and 64 per project, with `+` displayed on overflow). Calls without such an
ID are shown separately. These numbers do not identify models or measure all chats.
Catalog/help calls handled inside the MCP adapter and service monitoring polls are
excluded; client token counters are a different measurement. No historical status
or missing session information is fabricated.
