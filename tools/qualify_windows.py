#!/usr/bin/env python3
"""Standard-user source tests and exact Windows package qualification."""
import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'observer/tests'))


def cleanup_temporary(temporary):
    """Bound the release of owned Windows image locks; retain other failures."""
    deadline = time.monotonic() + 5
    while True:
        try:
            temporary.cleanup()
            return
        except PermissionError as error:
            if getattr(error, 'winerror', None) not in (32, 33) or time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


@contextmanager
def user_environment():
    """Do not inherit the runner administrator's TEMP into a different user."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    security = ctypes.WinDLL('advapi32', use_last_error=True)
    userenv = ctypes.WinDLL('userenv', use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    security.OpenProcessToken.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
    userenv.GetUserProfileDirectoryW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
    token = ctypes.c_void_p()
    assert security.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)), ctypes.get_last_error()
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_uint(len(buffer))
        assert userenv.GetUserProfileDirectoryW(token, buffer, ctypes.byref(size)), ctypes.get_last_error()
        profile = Path(buffer.value)
        assert profile.is_absolute() and profile.is_dir()
    finally:
        kernel.CloseHandle(token)
    # Short paths also keep the longest archive members below legacy Win32 limits.
    temporary = tempfile.TemporaryDirectory(prefix='s-', dir=profile)
    directory = temporary.name
    try:
        environment = os.environ.copy()
        environment.update(TEMP=directory, TMP=directory, TMPDIR=directory, USERPROFILE=str(profile),
                           HOMEDRIVE=profile.drive, HOMEPATH=str(profile)[len(profile.drive):])
        yield environment
    finally:
        # A stop reply acknowledges the request before the owned background
        # process releases its image. Wait only for Windows sharing locks on
        # this disposable tree; access/ownership errors and leaked locks fail.
        cleanup_temporary(temporary)


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


def qualification_receipt(output, identity):
    metadata = json.loads((output / 'package.json').read_text())
    runtime = json.loads((output / 'runtime.json').read_text())
    installed = json.loads((output / 'installed-preflight.json').read_text())
    assert all(metadata.get(k) == v and runtime.get(k) == v and installed.get(k) == v for k, v in identity.items())
    assert runtime.get('unelevated') is True
    assert runtime.get('render_scope') == 'headless'
    assert installed.get('unelevated') is True and installed.get('msi_root_verified') is True and installed.get('helper_staged') is True
    assert installed.get('version') == metadata['version'] and installed.get('core_version') == metadata['core_version']
    assert metadata['version'] == (ROOT / 'observer/VERSION').read_text().strip()
    assert metadata['core_version'] == (ROOT / 'VERSION').read_text().strip()
    return dict(schema_version='shin.windows-qualification.v1', **metadata, render_scope='headless',
                checks=['msi-metadata', 'msi-payload', 'archive-inventory', 'unelevated-runtime', 'installed-update-preflight'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('probe', 'features', 'package', 'runtime', 'installed-preflight', 'receipt'), required=True)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--msi', type=Path)
    parser.add_argument('--feature-test', type=Path)
    parser.add_argument('--cli', type=Path)
    parser.add_argument('--profiles', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--run-id', type=int, required=True)
    parser.add_argument('--attempt', type=int, required=True)
    args = parser.parse_args()
    assert sys.platform == 'win32' and re.fullmatch('[0-9a-f]{40}', args.sha)
    assert args.run_id > 0 and args.attempt > 0
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.phase in ('probe', 'features', 'runtime', 'installed-preflight'):
        elevated = bool(ctypes.windll.shell32.IsUserAnAdmin())
        (output / (args.phase + '-started.json')).write_text(json.dumps({'elevated': elevated}) + '\n')
        assert not elevated, 'Runtime qualification must be unelevated'
    if args.phase == 'probe':
        with user_environment() as environment:
            subprocess.run([sys.executable, '-c', 'import pathlib,tempfile; p=pathlib.Path(tempfile.gettempdir())/"probe"; p.write_text("ok"); p.unlink()'],
                           env=environment, check=True, timeout=10)
        (output / 'probe.json').write_text(json.dumps({'unelevated': True}) + '\n')
        return
    if args.phase == 'features':
        assert args.feature_test and args.cli and args.profiles, 'Feature test inputs are required'
        log_path = output / 'features.log'
        with user_environment() as environment, log_path.open('w', encoding='utf-8') as log:
            result = subprocess.run([str(args.feature_test.resolve()), str(args.cli.resolve()), str(args.profiles.resolve())],
                                    env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=45)
        if result.returncode:
            print(log_path.read_text(), file=sys.stderr)
            raise subprocess.CalledProcessError(result.returncode, result.args)
        (output / 'features.json').write_text(json.dumps(dict(unelevated=True, source_sha=args.sha,
            run_id=args.run_id, attempt=args.attempt, test='shin-windows-client-features')) + '\n')
        return
    assert args.archive and args.msi, 'Both package paths are required'
    archive, msi = args.archive.resolve(), args.msi.resolve()
    assets = hashes(archive, msi)
    identity = dict(source_sha=args.sha, run_id=args.run_id, attempt=args.attempt, assets=assets)
    if args.phase == 'package':
        manifest = package(archive, msi, output)
        proof = dict(identity, version=manifest['version'], core_version=manifest['core_version'])
    elif args.phase == 'runtime':
        with user_environment() as environment, (output / 'runtime.log').open('w', encoding='utf-8') as log:
            subprocess.run([sys.executable, '-X', 'utf8', '-B', str(ROOT / 'observer/tests/windows_kit_test.py'),
                            str(archive), '--headless-render'],
                           env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
        proof = dict(identity, unelevated=True, render_scope='headless')
    elif args.phase == 'installed-preflight':
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'Software\Shin\DesktopPreview',
                            access=winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            prefix = Path(winreg.QueryValueEx(key, 'InstallRoot')[0]).resolve()
        with user_environment() as environment:
            stage = Path(environment['TEMP']) / 'update preflight'
            subprocess.run([str(prefix / 'bin/shin-tray.exe'), '--update-preflight-test', str(stage)],
                           env=environment, check=True, timeout=45)
            report = json.loads((stage / 'state.json').read_text(encoding='utf-8'))
            assert report.get('ok') is True and report.get('msi_root_verified') is True and report.get('helper_staged') is True, report
            with zipfile.ZipFile(archive) as kit:
                manifest = json.loads(kit.read(archive.stem + '/shin-kit.json'))
                assert report['version'] == manifest['version']
                payload = {p.name: p for p in stage.iterdir() if p.suffix in ('.exe', '.dll')}
                expected = {'updater.exe': 'bin/shin-windows-updater.exe'}
                expected.update({Path(row['path']).name: row['path'] for row in manifest['files']
                                 if row['path'].startswith('bin/') and row['path'].endswith('.dll')})
                assert set(payload) == set(expected) and report['libraries'] == len(expected) - 1
                for name, relative in expected.items():
                    assert payload[name].read_bytes() == kit.read(archive.stem + '/' + relative), relative
        proof = dict(identity, unelevated=True, msi_root_verified=True, helper_staged=True,
                     version=manifest['version'], core_version=manifest['core_version'])
    else:
        receipt = qualification_receipt(output, identity)
        assert assets == hashes(archive, msi), 'Package changed during qualification'
        (archive.parent / 'windows-qualification.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print('Windows ZIP/MSI qualified; installed interactive-session lifecycle is a separate gate.')
        return
    assert assets == hashes(archive, msi), 'Package changed during qualification'
    (output / (args.phase + '.json')).write_text(json.dumps(proof, indent=2) + '\n')


if __name__ == '__main__':
    main()
