# Sand terminal monitor

`shin-ce stats` brings Monitor's Sand palette, project states and timing
calculations to an interactive terminal. The Sand TUI is included in preview
dev.39 and later; use the [preview kit](local-kit.md#download-a-preview-release)
on macOS or [build from source](building.md) on Linux.

Every image below comes from the actual FTXUI renderer with synthetic data.
Project names, paths, token counts, model labels and timings are examples,
not measurements of a real project or a performance claim. Your terminal
controls the font and available colors.

[Keyboard controls and usage](cli.md#interactive-terminal-monitor) ·
[Capture sources and reproduction](images/README.md#terminal-views)

## Overview

Memory, active queries and search p50 stay visible above a line graph and the
project table. Click a metric or press `g`; `h` cycles Live, one hour and 24 hours.
Missing observations remain gaps rather than becoming zero.

![Overview with memory graph and independent search and coverage states](images/tui-overview.svg)

## Projects

Availability and source coverage have separate columns. A project can be
searchable with partial coverage; preparing and on-demand readers have their own
states. Use arrows or `j`/`k` to select a row, then Enter for details.

![Project list with ready, partial, preparing, blocked and on-demand examples](images/tui-projects.svg)

## Project details

Details show the cause, next step and excluded files when they are reported.
This example excludes one synthetic credential-like file while keeping the rest
of the project searchable.

![Partial-coverage detail with a cause, next step and excluded example file](images/tui-project.svg)

## Timing

Search time, total service time and cold-reader preparation are shown separately.
`t` selects recent or retained measurements; `f` filters traffic. Diagnostic
requests stay identified, and missing attribution is not guessed.

![Timing with query counts, cold and warm reader measurements and phase means](images/tui-timing.svg)

## Storage

Storage shows usage, the configured budget and project caches. It is inspected
when this tab opens or when `r` is pressed. Cleanup remains an explicit CLI
operation with a reviewed revision.

![Storage inventory with project sizes and external-cache policy](images/tui-storage.svg)

## Models

The Models view reads the collector's normalized usage snapshot. Token counters
and model labels from client history are separate from attribution on individual
Shin requests. The example labels below are synthetic.

![Model usage with token totals, example models and unassigned sessions](images/tui-models.svg)

## English and Russian

Press `l` to change the current terminal session immediately, or launch with
`--language en` / `--language ru`. Automatic selection follows the terminal locale.

![The same overview rendered in Russian](images/tui-overview-ru.svg)

## Compact terminals

At 80×24 cells, the dashboard retains the main metrics, line graph and project
states. Extra width adds the observer column. The minimum is 60×12 cells;
noninteractive sessions can use text or JSON status instead.

![Compact 80 by 24 terminal overview](images/tui-compact.svg)

## Disconnected service

When the service becomes unavailable, the last received observations remain
visible and are marked stale. The TUI reconnects automatically; it does not start
another service or change project enrollment.

![Disconnected view with stale retained observations](images/tui-offline.svg)
