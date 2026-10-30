#!/usr/bin/env python3
"""Export a native FTXUI fixture grid as SVG; no service or user data is read."""
import argparse
import html
import json
from pathlib import Path
import re


def rgb(value, fallback):
    match = re.fullmatch(r'(?:38|48);2;(\d+);(\d+);(\d+)', value)
    return '#' + ''.join(f'{int(n):02x}' for n in match.groups()) if match else fallback


GALLERY = {
    'overview-en': 'tui-overview.svg',
    'projects-en': 'tui-projects.svg',
    'project-detail-en': 'tui-project.svg',
    'timing-en': 'tui-timing.svg',
    'storage-en': 'tui-storage.svg',
    'models-en': 'tui-models.svg',
    'journal-en': 'tui-journal.svg',
    'settings-en': 'tui-settings.svg',
    'settings-ru': 'tui-settings-ru.svg',
    'overview-ru': 'tui-overview-ru.svg',
    'compact-en': 'tui-compact.svg',
    'offline-en': 'tui-offline.svg',
    'updates-offer-en': 'tui-updates.svg',
    'updates-progress-en': 'tui-update-progress.svg',
}


def export(grid, destination):
    rows = json.loads(grid.read_text())
    assert rows and len(rows) <= 256
    columns = len(rows[0])
    assert 0 < columns <= 512 and all(len(row) == columns for row in rows)
    cell_width, cell_height, margin = 8.5, 20, 20
    width, height = columns * cell_width + margin * 2, len(rows) * cell_height + margin * 2
    background, foreground = '#1d1f1c', '#f0efe3'
    output = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:g}" height="{height:g}" viewBox="0 0 {width:g} {height:g}">',
              f'<title>Shin Sand terminal monitor: {html.escape(grid.stem)}</title>',
              '<desc>Native FTXUI synthetic fixture. Colors, symbols and text distinguish state and coverage. These are fixture values, not performance measurements.</desc>',
              f'<rect width="{width:g}" height="{height:g}" fill="{background}"/>',
              '<g font-family="Menlo,Consolas,monospace" font-size="14">']
    for y, row in enumerate(rows):
        cells = []
        for x, (character, fg, bg, bold, inverted) in enumerate(row):
            fg, bg = rgb(fg, foreground), rgb(bg, background)
            if inverted:
                fg, bg = bg, fg
            cells.append((character, fg, bg, bold))
        start = 0
        while start < columns:
            end = start + 1
            while end < columns and cells[end][2] == cells[start][2]:
                end += 1
            bg = cells[start][2]
            if bg != background:
                output.append(f'<rect x="{margin + start * cell_width:g}" y="{margin + y * cell_height:g}" width="{(end - start) * cell_width:g}" height="{cell_height:g}" fill="{bg}"/>')
            start = end
        for x, (character, fg, bg, bold) in enumerate(cells):
            xx, yy = margin + x * cell_width, margin + y * cell_height
            if character.strip():
                output.append(f'<text x="{xx:g}" y="{yy + 15:g}" fill="{fg}" font-weight="{700 if bold else 400}">{html.escape(character)}</text>')
    output.extend(['</g>', '</svg>'])
    destination.write_text('\n'.join(output) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gallery', action='store_true', help='export all named fixture views to a directory')
    parser.add_argument('grid', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.gallery:
        args.destination.mkdir(parents=True, exist_ok=True)
        for name, output in GALLERY.items():
            export(args.grid / (name + '.json'), args.destination / output)
    else:
        export(args.grid, args.destination)


if __name__ == '__main__':
    main()
