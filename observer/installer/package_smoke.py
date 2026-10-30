#!/usr/bin/env python3
"""Qualify an exact macOS kit archive using only isolated installs and data."""
import argparse
from contextlib import contextmanager
import os
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
completed = 0
stage_count = 8


@contextmanager
def stage(name):
    global completed
    print(f'Kit: {completed}/{stage_count} ({completed * 100 // stage_count}%) — starting {name}', flush=True)
    status = 'passed'
    try:
        yield
    except BaseException:
        status = 'failed'
        raise
    else:
        completed += 1
    finally:
        volume = f'{completed}/{stage_count} ({completed * 100 // stage_count}%)'
        print(f'Kit: {volume} — {name}: {status}', flush=True)
        if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
            with Path(summary).open('a') as stream:
                stream.write(f'- Kit {volume}: {name} — **{status}**\n')


with stage('EN/RU GUI proof'):
    results = ET.parse(args.test_results).getroot()
    for name in ('shin-tray-layout', 'shin-tray-layout-en'):
        matches = [test for test in results.iter('testcase') if test.get('name') == name]
        assert len(matches) == 1, f'{name}: missing GUI proof'
        case = matches[0]
        assert case.find('skipped') is None and case.find('failure') is None and case.find('error') is None, f'{name}: GUI proof did not pass'

def run(script, *options):
    subprocess.run([sys.executable, '-B', str(source / script), *map(str, options)], check=True, timeout=300)

with stage('Relocated installation and retrieval'):
    run('tests/local_install_test.py', args.cmake, args.build, '1', '--archive', args.archive)
with stage('Native installation'):
    run('tests/native_installation_test.py', args.cmake, args.build, '--archive', args.archive)
with stage('Native update and recovery'):
    run('installer/update_test.py', args.cmake, args.build, '--archive', args.archive)
with stage('Bootstrap installer'):
    run('installer/bootstrap_test.py', args.build / 'packages/install.sh', args.archive)
with stage('Previous generator update and rollback'):
    subprocess.run([sys.executable, '-B', str(source / 'installer/previous_generator_test.py'),
                    '--cmake', args.cmake, '--candidate-archive', str(args.archive.resolve()),
                    '--proof', str(args.build / 'proof')], check=True, timeout=600)
with stage('Independent updater envelope'):
    updater = args.archive.parent / 'shin-updater-v1-macos-arm64.zip'
    subprocess.run([str(args.build / 'shin-updater-bundle-tests'), 'archive', str(updater)], check=True, timeout=15)
    with tempfile.TemporaryDirectory(prefix='shin-updater.', dir='/tmp') as folder:
        payload = Path(folder).resolve() / 'payload'
        payload.mkdir(mode=0o700)
        subprocess.run([args.cmake, '-E', 'tar', 'xf', str(updater.resolve())], cwd=payload, check=True, timeout=30)
        bundle = payload / 'shin-updater-v1'
        subprocess.run([str(args.build / 'shin-updater-bundle-tests'), 'bundle', str(bundle)], check=True, timeout=15)
        subprocess.run([str(bundle / 'bin/shin-updater'), '--protocol'], check=True, timeout=10)
        probe = subprocess.run([str(bundle / 'bin/shin-updater'), 'update-check', '--prefix', str(payload / 'absent')],
                               capture_output=True, text=True, timeout=15)
        assert probe.returncode != 0 and bundle.exists(), 'read-only probe deleted the pending updater envelope'
with stage('EN/RU terminal stats and restoration'):
    with tempfile.TemporaryDirectory(prefix='shin-package-stats-') as folder:
        # Preserve executable modes from the exact archive.
        subprocess.run([args.cmake, '-E', 'tar', 'xf', str(args.archive.resolve())], cwd=folder, check=True, timeout=90)
        kits = list(Path(folder).glob('*/shin-kit.json'))
        assert len(kits) == 1
        kit = kits[0].parent
        run('tui/terminal_test.py', kit / 'bin/shin-ce', kit / 'share/shin-context/profiles')
print('Exact kit: relocated and native installation, query/read, EN/RU stats and terminal restoration passed')
