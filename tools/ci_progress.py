#!/usr/bin/env python3
"""Stream native CI logs with separate operation counters; preserve command exits."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

BUILD = re.compile(r'^\[Build (\d+)/(\d+)\s+\d+%\]')
TEST = re.compile(r'^\s*(\d+)/(\d+)\s+Test\s+#?\d+:')


def run(kind, command):
    env = dict(os.environ)
    if kind == 'build':
        env['NINJA_STATUS'] = '[Build %f/%t %p] '
    completed = total = failed = skipped = 0
    no_work = False
    with subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1) as child:
        for line in child.stdout:
            print(line, end='', flush=True)
            no_work |= 'ninja: no work to do.' in line
            match = (BUILD if kind == 'build' else TEST).match(line)
            if not match:
                continue
            completed, total = map(int, match.groups())
            if kind == 'tests':
                if '***Skipped' in line or '***Not Run (Disabled)' in line:
                    skipped += 1
                elif 'Passed' not in line:
                    failed += 1
                print(f'Tests: {completed}/{total} ({completed * 100 // total}%) '
                      f'— failed: {failed}, skipped: {skipped}', flush=True)
        code = child.wait()
    status = 'passed' if code == 0 else f'failed (exit {code})'
    volume = f'{completed}/{total} ({completed * 100 // total}%)' if total else 'no counter reported'
    if kind == 'build' and code == 0 and not total and no_work:
        volume = 'no pending operations'
    detail = f'; failed: {failed}, skipped: {skipped}' if kind == 'tests' else ''
    result = f'{kind.title()}: {volume} — {status}{detail}'
    print(result, flush=True)
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(summary).open('a') as stream:
            stream.write(f'### {kind.title()}\n\n{volume} — **{status}**{detail}\n\n'
                         'Counts describe completed operations, not elapsed or remaining time.\n\n')
    return code if code >= 0 else 128 - code


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('build', 'tests'))
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a command is required after --')
    sys.exit(run(args.kind, command))
