# <img src="docs/images/shin-logo.png" align="absmiddle" width="48" height="48" alt="Shin logo"> Shin Context Engine

[![Source qualification](https://github.com/toseek-samurai/shin-context-engine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/toseek-samurai/shin-context-engine/actions/workflows/ci.yml)

**Keep project context current. Give coding agents evidence they can inspect.**

Shin indexes your project's source and documentation, watches for changes, and
returns focused context through a native CLI or MCP. Results carry a snapshot
that the client can use to inspect and read the matching source.

The local macOS kit includes the background service and **Shin Monitor**, a menu
bar app for project health, request history, timings, storage, and optional model
usage. Prefer a terminal? `shin-ce stats` opens an interactive monitor with the same
project states and timing calculations. Local retrieval needs no account, model
download, or cloud inference API.

**Development preview:** the source is available for review and testing.
There is no stable release yet. Development focuses on the portable local engine,
its CLI and MCP integrations, and the Monitor interface.

[Get started](#get-started-on-macos) · [CLI guide](docs/cli.md) ·
[Connect an MCP client](docs/native-client-setup.md) ·
[Build from source](docs/building.md) · [Architecture](docs/architecture.md)

[Report a bug or suggest a feature](https://github.com/toseek-samurai/shin-context-engine/issues/new/choose) ·
[Contribute](CONTRIBUTING.md)

## See your project's state

<p>
  <img src="docs/images/monitor-projects.png" width="280" alt="Shin Monitor lists projects and their current search and observer status">
  <img src="docs/images/monitor-project.png" width="280" alt="Project detail shows current context, the last check, updates and query history">
  <img src="docs/images/monitor-timing.png" width="280" alt="Request timing separates search time from total service time and preparation">
</p>

Real Monitor screens rendered with synthetic sample data. Monitor supports
English and Russian; these examples use English. Names, counts, and timings above are examples, not benchmark results.
[Screenshot sources and reproduction](docs/images/README.md).

- **See freshness and failures.** Monitor distinguishes current, preparing,
  partial, and blocked project states, with a next step when action is needed.
- **Ask for focused evidence.** Query from your terminal or coding agent, then
  inspect and read using the returned snapshot.
- **Keep state outside your repository.** The native kit stores indexes and
  history in a separate private data directory. Adding a project does not add
  configuration files to its source tree.
- **Use the same local service from UI and CLI.** The TUI has overview, projects,
  timing, storage, and model-usage tabs. Text and JSON status remain available. See the
  [UI-to-CLI map](docs/cli.md#monitor-and-cli).

## Also in your terminal

<img src="docs/images/tui-projects.svg" width="864" alt="Shin TUI with Overview, Projects, Timing, Storage and Models tabs; two example projects are ready">

`shin-ce stats` on two disposable example projects. This is a captured terminal
view; colors and fonts follow your terminal. [Keyboard controls and usage
setup](docs/cli.md#interactive-terminal-monitor).

## Get started on macOS

The preview includes a first-run setup window, automatic agent connections
and project statistics; see [guided setup](docs/native-client-setup.md#automatic-setup).

The [development preview](https://github.com/toseek-samurai/shin-context-engine/releases/tag/v2.0.5-dev.14)
is available for **macOS 15+ on Apple Silicon**. Install or update with:

```sh
curl -fsSL https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.14/install.sh | sh
```

The installer downloads a versioned binary kit, checks SHA-256, and installs it
for your user without `sudo` or a compiler. This preview is ad-hoc signed and
not notarized; macOS may require manual approval on first launch. You can also
[download and inspect the archive](docs/local-kit.md#download-a-preview-release),
[build from source](docs/local-kit.md#build-the-package), or test a
[CI candidate](docs/local-kit.md#download-a-ci-candidate).

Connect your first project:

```sh
export PATH="$HOME/.local/bin:$PATH"

cd /path/to/your/project
shin-ce init
shin-ce status --text
shin-ce query "Where are requests validated?"
```

`init` adds the current project and starts the local service. Initial preparation
can take time; use `status --text` to see whether search is ready. The `PATH`
change above applies to this terminal session; installation leaves your shell
startup files unchanged.

Open Monitor and click its menu bar icon:

```sh
open "$HOME/.local/opt/shin-local/Shin Monitor.app"
```

You can add further folders in Monitor or with
`shin-ce project-add --root /path/to/another/project`. For terminal-only use, run
`shin-ce stats` (or `shin-ce service-status --text` for a single status report).
Use Tab or 1–5 to switch views, Enter for project details, and q to exit.
To use Shin from a coding agent,
[configure the included MCP adapter](docs/native-client-setup.md). Automatic
startup at login is a separate opt-in step in the
[installation guide](docs/local-kit.md#automatic-startup). For an existing kit,
follow the [update procedure](docs/local-kit.md#update-an-existing-kit), which preserves
project data and restores the previous kit if activation fails.

## Choose the component you need

| Component | What it does | Current distribution scope |
| --- | --- | --- |
| Local kit | Core, Observer, CLI, MCP, usage collector, and Monitor | macOS 15+ Apple Silicon preview; verified binary kit |
| Core | Indexing and bounded retrieval for tools and integrations | macOS and Linux source builds; [build instructions](docs/building.md) |

Linux desktop packaging, Windows distribution, and macOS Intel package
qualification remain pending. Configuring an AI client may send the context it retrieves to that
client's chosen model provider.

For contributors: [build and test](docs/building.md),
[contribution guidance](CONTRIBUTING.md), and the
[product manifest](docs/machine/shin-context-engine.manifest.json).
The root `VERSION` records [Core](VERSION); [Observer](observer/VERSION) is
versioned separately.

Licensed under [Apache-2.0](LICENSE). [Third-party notices](THIRD_PARTY.md)
and [private security reporting](SECURITY.md).
