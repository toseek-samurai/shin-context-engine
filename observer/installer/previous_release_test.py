#!/usr/bin/env python3
"""Qualify an exact candidate archive against the pinned published previous release."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from pinned_archive import download_pinned_archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--candidate-archive', type=Path, required=True)
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    previous = json.loads((root / '.github/fixtures/macos-previous-release.json').read_text())
    args.proof.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shin-previous-release-') as temporary:
        folder = Path(temporary)
        archive = folder / previous['archive']
        download_pinned_archive(previous, archive)
        old = folder / 'old'; old.mkdir()
        new = folder / 'new'; new.mkdir()
        for path, destination in ((archive, old), (args.candidate_archive.resolve(), new)):
            subprocess.run([args.cmake, '-E', 'tar', 'xf', str(path)], cwd=destination, check=True, timeout=90)
        old_kits = list(old.glob('*/shin-kit.json')); new_kits = list(new.glob('*/shin-kit.json'))
        assert len(old_kits) == len(new_kits) == 1
        previous_kit, candidate_kit = old_kits[0].parent, new_kits[0].parent
        prior_manifest=json.loads((previous_kit/'shin-kit.json').read_text())
        target_manifest=json.loads((candidate_kit/'shin-kit.json').read_text())
        if prior_manifest['core_version'] == target_manifest['core_version']:
            subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('same_core_update_test.py')),
                            '--previous-kit', str(previous_kit), '--candidate-kit', str(candidate_kit),
                            '--report', str(args.proof / 'previous-release-update.json')],
                           check=True, timeout=300)
        else:
            for scenario in ('forward', 'rollback-resume'):
                subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('format_transition_test.py')),
                                '--previous-kit', str(previous_kit), '--candidate-kit', str(candidate_kit),
                                '--scenario', scenario, '--with-marker',
                                '--report', str(args.proof / ('previous-release-'+scenario+'.json'))],
                               check=True, timeout=300)
        subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('mcp_handoff_test.py')),
                        '--previous-kit', str(previous_kit), '--candidate-kit', str(candidate_kit),
                        '--report', str(args.proof / 'previous-release-mcp-handoff.json')],
                       check=True, timeout=300)
        subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('update_recovery_test.py')),
                        '--helper', str(args.helper.resolve()), '--previous-kit', str(previous_kit),
                        '--candidate-kit', str(candidate_kit), '--archive', str(args.candidate_archive.resolve()),
                        '--report', str(args.proof / 'previous-release-recovery.json')],
                       check=True, timeout=600)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
