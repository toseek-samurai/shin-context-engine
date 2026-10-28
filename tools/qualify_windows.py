#!/usr/bin/env python3
"""Exact Windows package gate. MSI extraction does not install or enroll projects."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'observer/tests'))


def hashes(archive, msi):
    paths = (archive, msi, archive.with_suffix('.zip.sha256'), msi.with_suffix('.msi.sha256'))
    result = {p.name: 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    for path in (archive, msi):
        assert path.with_suffix(path.suffix + '.sha256').read_text().strip() == \
            f'{result[path.name][7:]}  {path.name}', 'Package checksum mismatch'
    return result


def package(archive, msi, output):
    from windows_kit_test import check_msi
    properties = check_msi(msi)
    with tempfile.TemporaryDirectory(prefix='shin-msi-payload-') as temporary:
        extracted = Path(temporary) / 'image'
        log = output / 'msi-extraction.log'
        subprocess.run(['msiexec.exe', '/a', str(msi), '/qn', '/norestart',
                        'TARGETDIR=' + str(extracted), '/L*V', str(log)], check=True, timeout=180)
        manifests = list(extracted.rglob('shin-kit.json'))
        assert len(manifests) == 1, 'Expected one MSI payload'
        with zipfile.ZipFile(archive) as kit:
            prefix = archive.stem + '/'
            manifest_bytes = kit.read(prefix + 'shin-kit.json')
            manifest = json.loads(manifest_bytes)
            assert manifest['version'] == properties['SHIN_PREVIEW_VERSION']
            assert manifests[0].read_bytes() == manifest_bytes, 'MSI and ZIP manifests differ'
            for row in manifest['files']:
                relative = row['path']
                assert not Path(relative).is_absolute() and '..' not in Path(relative).parts
                assert ':' not in relative and '\\' not in relative
                value = (manifests[0].parent / relative).read_bytes()
                assert value == kit.read(prefix + relative), 'MSI and ZIP payloads differ: ' + relative
                assert len(value) == row['size'] and hashlib.sha256(value).hexdigest() == row['sha256']
            actual = {p.relative_to(manifests[0].parent).as_posix() for p in manifests[0].parent.rglob('*') if p.is_file()}
            assert actual == {'shin-kit.json', *(r['path'] for r in manifest['files'])}, 'Unlisted MSI payload'
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('package', 'runtime', 'receipt'), required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--msi', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--run-id', type=int, required=True)
    parser.add_argument('--attempt', type=int, required=True)
    args = parser.parse_args()
    assert sys.platform == 'win32' and re.fullmatch('[0-9a-f]{40}', args.sha)
    assert args.run_id > 0 and args.attempt > 0
    archive, msi, output = args.archive.resolve(), args.msi.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    assets = hashes(archive, msi)
    identity = dict(source_sha=args.sha, run_id=args.run_id, attempt=args.attempt, assets=assets)
    if args.phase == 'package':
        manifest = package(archive, msi, output)
        proof = dict(identity, version=manifest['version'], core_version=manifest['core_version'])
    elif args.phase == 'runtime':
        assert not ctypes.windll.shell32.IsUserAnAdmin(), 'Runtime qualification must be unelevated'
        with (output / 'runtime.log').open('w', encoding='utf-8') as log:
            subprocess.run([sys.executable, '-X', 'utf8', '-B', str(ROOT / 'observer/tests/windows_kit_test.py'), str(archive)],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
        proof = dict(identity, unelevated=True)
    else:
        metadata = json.loads((output / 'package.json').read_text())
        runtime = json.loads((output / 'runtime.json').read_text())
        assert all(metadata.get(k) == v and runtime.get(k) == v for k, v in identity.items())
        assert runtime.get('unelevated') is True
        assert metadata['version'] == (ROOT / 'observer/VERSION').read_text().strip()
        assert metadata['core_version'] == (ROOT / 'VERSION').read_text().strip()
        receipt = dict(schema_version='shin.windows-qualification.v1', **metadata,
                       checks=['msi-metadata', 'msi-payload', 'archive-inventory', 'unelevated-runtime'])
        (archive.parent / 'windows-qualification.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print('Windows ZIP/MSI qualified; installed interactive-session lifecycle is a separate gate.')
        return
    assert assets == hashes(archive, msi), 'Package changed during qualification'
    (output / (args.phase + '.json')).write_text(json.dumps(proof, indent=2) + '\n')


if __name__ == '__main__':
    main()
