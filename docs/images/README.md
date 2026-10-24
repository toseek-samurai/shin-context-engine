# UI screenshots

These PNGs are rendered by the actual macOS ImGui/Metal Monitor UI using the
synthetic scenarios in [layout_test.inc](../../observer/tray/layout_test.inc).
No live service, enrolled project, client usage history, or saved user preference
is read. Displayed metrics are fixture values, not performance measurements.

| README asset | Native fixture output |
| --- | --- |
| `monitor-projects.png` | `project-states-448x620.png` |
| `monitor-project.png` | `current-448x620.png` |
| `monitor-timing.png` | `timing-retained-448x620.png` |

After building the macOS kit with testing enabled, render again:

```sh
/tmp/shin-observer-kit/tray/shin-tray-layout-tests \
  /tmp/shin-monitor-screenshots -ShinHome /shin-monitor-test-home en
```

Metal must be available; a skipped render is not a new screenshot. Inspect the
outputs visually, then copy only the three selected PNGs here without modifying
their pixels. Each image is rendered at 2x resolution. The screenshots use English; pass `ru` instead of `en` to render Russian.
Both languages use the same native views and synthetic scenarios.
The images show the dev.34 Sand appearance at the standard 448×620 panel size.
The project list deliberately mixes ready, preparing, partial, blocked, on-demand,
waiting and recovering states to demonstrate their distinct colors and symbols.
The blocked project and displayed counters are synthetic examples. The
compact layout remains available. Memory curves retain observation gaps and
service-run boundaries.

## Terminal views

`tui-overview.svg` is the native FTXUI Overview renderer at 120×36 cells,
using the independent synthetic fixture in
[`sand_test.cpp`](../../observer/tui/sand_test.cpp). The same view is used by
`shin-ce stats`; no browser approximation, live service or private project is
used. The screenshot demonstrates ready, partial, preparing, blocked and
on-demand states. Its counters are fixture values.

The [screen gallery](../tui.md) also includes Projects, partial-coverage details,
Timing, Storage, Models, Russian, compact and disconnected views. These show
the Sand TUI included in preview dev.39 and later.
All names, paths, tokens and counts are synthetic; example model labels do not
represent real client attribution.

After building Observer with testing enabled, refresh the gallery with:

```sh
/path/to/build/shin-tui-render-tests /path/to/disposable-captures
python3 observer/tui/capture.py --gallery /path/to/disposable-captures docs/images
```

The exporter preserves native cell text, colors, selection and weight.
Its only dependency is Python's standard library. Inspect the result visually;
terminal font and color capabilities still affect the actual application.
The JSON, plain-text and ANSI frame exports stay in the disposable capture
directory; only the selected SVGs belong in this documentation.

## Project logo

`shin-logo.png` is the existing Shin project mark. PNG text metadata records
Shin Context Engine, Anton Petropavlovskiy and Apache-2.0. It contains no source
file paths or inherited EXIF/XMP metadata. The original pixel stream is preserved.
