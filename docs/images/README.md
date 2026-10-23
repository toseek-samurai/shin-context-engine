# Monitor screenshots

These PNGs are rendered by the actual macOS ImGui/Metal Monitor UI using the
synthetic scenarios in [layout_test.inc](../../observer/tray/layout_test.inc).
No live service, enrolled project, client usage history, or saved user preference
is read. Displayed metrics are fixture values, not performance measurements.

| README asset | Native fixture output |
| --- | --- |
| `monitor-projects.png` | `projects-448x620.png` |
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
The images show the Sand appearance at the standard 448×620 panel size; the
compact layout remains available. Memory curves retain observation gaps and
service-run boundaries.

## Terminal view

`tui-projects.svg` captures a real `shin-ce stats` session in a 96-column,
18-row pseudo-terminal. Two disposable folders, Example API and Example Web,
each contain a synthetic `requestLimit` function and are enrolled in a separate
temporary data home. The Projects tab is captured after a query makes each
reader ready. No installed service or private project is used.

The ANSI screen is decoded with pyte 0.8.2 and exported as SVG text and cell
colors. Pyte is a capture-time tool, not a Shin runtime or test dependency.
The dark palette and monospace font represent one terminal theme; actual colors
and fonts depend on the user's terminal. To refresh the image, repeat the same
disposable setup, open `shin-ce stats --home /absolute/path/to/demo-state`, choose
Projects, and capture the terminal view. Stop only the demo service afterward.

## Project logo

`shin-logo.png` is the existing Shin project mark. PNG text metadata records
Shin Context Engine, Anton Petropavlovskiy and Apache-2.0. It contains no source
file paths or inherited EXIF/XMP metadata. The original pixel stream is preserved.
