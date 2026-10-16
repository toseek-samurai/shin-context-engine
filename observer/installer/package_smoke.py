#!/usr/bin/env python3
"""Qualify an exact macOS kit archive using only isolated installs and data."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--cmake', required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--test-results', type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
results = ET.parse(args.test_results).getroot()
for name in ('shin-tray-layout', 'shin-tray-layout-en'):
    matches = [test for test in results.iter('testcase') if test.get('name') == name]
    assert len(matches) == 1, f'{name}: missing GUI proof'
    case = matches[0]
    assert case.find('skipped') is None and case.find('failure') is None and case.find('error') is None, f'{name}: GUI proof did not pass'

def run(script, *options):
    subprocess.run([sys.executable, '-B', str(source / script), *map(str, options)], check=True, timeout=300)

run('tests/local_install_test.py', args.cmake, args.build, '1', '--archive', args.archive)
run('tests/native_installation_test.py', args.cmake, args.build, '--archive', args.archive)
with tempfile.TemporaryDirectory(prefix='shin-package-stats-') as folder:
    # Preserve executable modes from the exact archive.
    subprocess.run([args.cmake, '-E', 'tar', 'xf', str(args.archive.resolve())], cwd=folder, check=True, timeout=90)
    kits = list(Path(folder).glob('*/shin-kit.json'))
    assert len(kits) == 1
    kit = kits[0].parent
    run('tui/terminal_test.py', kit / 'bin/shin-ce', kit / 'share/shin-context/profiles')
print('Exact kit: relocated and native installation, query/read, EN/RU stats and terminal restoration passed')
