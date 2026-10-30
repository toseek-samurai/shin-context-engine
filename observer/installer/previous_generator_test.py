#!/usr/bin/env python3
"""Qualify an exact candidate archive against the pinned published retired generator."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--candidate-archive', type=Path, required=True)
    parser.add_argument('--proof', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    previous = json.loads((root / '.github/fixtures/macos-previous-generator.json').read_text())
    args.proof.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shin-previous-generator-') as temporary:
        folder = Path(temporary)
        archive = folder / previous['archive']
        subprocess.run(['/usr/bin/curl', '--fail', '--location', '--proto', '=https', '--proto-redir', '=https',
                        '--retry', '2', '--connect-timeout', '15', '--max-time', '120',
                        '--max-filesize', str(previous['bytes']), '--output', str(archive), previous['url']],
                       check=True, timeout=300)
        assert archive.stat().st_size == previous['bytes']
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == previous['sha256']
        old = folder / 'old'; old.mkdir()
        new = folder / 'new'; new.mkdir()
        for path, destination in ((archive, old), (args.candidate_archive.resolve(), new)):
            subprocess.run([args.cmake, '-E', 'tar', 'xf', str(path)], cwd=destination, check=True, timeout=90)
        old_kits = list(old.glob('*/shin-kit.json')); new_kits = list(new.glob('*/shin-kit.json'))
        assert len(old_kits) == len(new_kits) == 1
        previous_kit, candidate_kit = old_kits[0].parent, new_kits[0].parent
        identity = json.loads(subprocess.check_output([str(previous_kit / 'bin/shin-context'), '--version'], text=True))
        assert identity['version'] == previous['core_version'] and identity['generator_version'] == previous['generator']
        for scenario in ('forward', 'rollback-resume', 'profile-policy-rejected'):
            command = [sys.executable, '-B', str(Path(__file__).with_name('format_transition_test.py')),
                       '--previous-kit', str(previous_kit), '--candidate-kit', str(candidate_kit),
                       '--scenario', scenario, '--report', str(args.proof / f'previous-generator-{scenario}.json')]
            if scenario == 'forward': command.append('--fresh-updater')
            subprocess.run(command, check=True, timeout=300)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
