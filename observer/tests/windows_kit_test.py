"""Validate the actual Windows archive; no real user enrollment or MSI install."""
import argparse
import ctypes
import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import zipfile


def check_msi(path):
    """Read MSI tables without starting installation or changing machine state."""
    api = ctypes.WinDLL('msi', use_last_error=True)
    api.MsiOpenDatabaseW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
    api.MsiDatabaseOpenViewW.argtypes = [ctypes.c_uint, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
    api.MsiViewExecute.argtypes = [ctypes.c_uint, ctypes.c_uint]
    api.MsiViewFetch.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
    api.MsiRecordGetStringW.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
    api.MsiCloseHandle.argtypes = [ctypes.c_uint]
    database = ctypes.c_uint()
    assert api.MsiOpenDatabaseW(str(path.resolve()), None, ctypes.byref(database)) == 0

    def rows(query, columns):
        view = ctypes.c_uint()
        assert api.MsiDatabaseOpenViewW(database, query, ctypes.byref(view)) == 0
        try:
            assert api.MsiViewExecute(view, 0) == 0
            result = []
            while True:
                record = ctypes.c_uint()
                code = api.MsiViewFetch(view, ctypes.byref(record))
                if code == 259:
                    return result
                assert code == 0, code
                try:
                    values = []
                    for column in range(1, columns + 1):
                        buffer = ctypes.create_unicode_buffer(4096)
                        size = ctypes.c_uint(len(buffer))
                        assert api.MsiRecordGetStringW(record, column, buffer, ctypes.byref(size)) == 0
                        values.append(buffer.value)
                    result.append(values)
                finally:
                    api.MsiCloseHandle(record)
        finally:
            api.MsiCloseHandle(view)

    try:
        properties = dict(rows('SELECT `Property`, `Value` FROM `Property`', 2))
        assert properties.get('ALLUSERS', '') == '1', properties.get('ALLUSERS')
        assert properties['ProductName'] == 'Shin Desktop Preview'
        directories = {item[0]: item[1] for item in rows('SELECT `Directory`, `Directory_Parent` FROM `Directory`', 2)}
        parent = 'INSTALL_ROOT'
        visited = set()
        while parent and parent != 'ProgramFiles64Folder':
            assert parent not in visited
            visited.add(parent)
            parent = directories[parent]
        assert parent == 'ProgramFiles64Folder', directories
        registry_roots = rows('SELECT `Root` FROM `Registry`', 1)
        assert registry_roots and all(row == ['2'] for row in registry_roots), registry_roots
        shortcuts = rows('SELECT `Name` FROM `Shortcut`', 1)
        assert any('Shin Monitor' in row[0] for row in shortcuts), shortcuts
        actions = {row[0]: row[1:] for row in rows('SELECT `Action`, `Type`, `Source`, `Target` FROM `CustomAction`', 4)}
        assert 'v' in properties.get('MsiLogging', ''), 'MSI must retain native diagnostics on double-click install'
        assert properties['WIXUI_EXITDIALOGOPTIONALCHECKBOX'] == '1'
        assert properties['WixUnelevatedShellExecTarget'] == '[#CM_FP_bin.shin_tray.exe]'
        assert actions['ShinLaunchMonitor'][1:] == ['Wix4UtilCA_X64', 'WixUnelevatedShellExec']
        assert not int(actions['ShinLaunchMonitor'][0]) & (0x400 | 0x800)
        launch = rows("SELECT `Dialog_`, `Control_`, `Event`, `Argument`, `Condition` FROM `ControlEvent` WHERE `Argument` = 'ShinLaunchMonitor'", 5)
        assert len(launch) == 1 and launch[0][:4] == ['ExitDialog', 'Finish', 'DoAction', 'ShinLaunchMonitor']
        assert 'NOT Installed' in launch[0][4] and 'WIXUI_EXITDIALOGOPTIONALCHECKBOX' in launch[0][4]
        sequence = {row[0]: int(row[1]) for row in rows('SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`', 2)}
        for name, command, rollback in (('ShinScmInstall', '_scm-install', False),
                                        ('ShinScmStop', '_scm-stop', False),
                                        ('ShinScmRemove', '_scm-uninstall', False),
                                        ('ShinScmRestore', '_scm-install', True),
                                        ('ShinScmUndoInstall', '_scm-uninstall', True)):
            kind, source, target = actions[name]
            assert int(kind) & 0x400 and int(kind) & 0x800, (name, kind)
            assert bool(int(kind) & 0x100) == rollback, (name, kind)
            assert source == 'Wix4UtilCA_X64' and target == 'WixQuietExec', actions[name]
            setters = [key for key, row in actions.items() if int(row[0]) == 51 and row[1] == name
                       and row[2] == f'"[#CM_FP_bin.shin_ce.exe]" {command}']
            assert len(setters) == 1 and sequence[setters[0]] < sequence[name], ('missing scheduled native command', name)
        assert sequence['ShinScmRestore'] < sequence['ShinScmStop'] < sequence['RemoveFiles']
        assert sequence['ShinScmStop'] < sequence['ShinScmRemove'] < sequence['RemoveFiles']
        assert sequence['InstallFiles'] < sequence['ShinScmInstall']
        assert sequence['ShinScmUndoInstall'] < sequence['ShinScmInstall']
        startup = rows("SELECT `Name`, `Value` FROM `Registry` WHERE `Key` = 'Software\\Microsoft\\Windows\\CurrentVersion\\Run'", 2)
        assert ['ShinMonitor', '"[#CM_FP_bin.shin_tray.exe]"'] in startup, startup
        print('MSI metadata: per-machine, Program Files, HKLM, native SCM actions/rollback, separate Monitor startup passed')
        return properties
    finally:
        api.MsiCloseHandle(database)


def run(command, timeout=90):
    value = subprocess.run(list(map(str, command)), capture_output=True,
                           encoding='utf-8', timeout=timeout)
    assert value.returncode == 0, (command, value.returncode, value.stdout, value.stderr)
    return value.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--msi', type=Path)
    parser.add_argument('--keep', action='store_true', help='Retain this disposable fixture for failure diagnosis')
    args = parser.parse_args()
    assert os.name == 'nt', 'Run against native Windows binaries'
    if args.msi:
        check_msi(args.msi)
    storage = contextlib.nullcontext(tempfile.mkdtemp(prefix='shin-windows-kit-')) if args.keep else tempfile.TemporaryDirectory(prefix='shin-windows-kit-')
    with storage as temporary:
        base = Path(temporary)
        if args.keep:
            print(f'Fixture retained: {base}', flush=True)
        with zipfile.ZipFile(args.archive) as archive:
            for info in archive.infolist():
                parts = Path(info.filename).parts
                assert not Path(info.filename).is_absolute() and '..' not in parts
            archive.extractall(base / 'unpacked')
        manifests = list((base / 'unpacked').glob('*/shin-kit.json'))
        assert len(manifests) == 1, 'Expected one complete kit'
        kit = base / 'relocated kit'
        manifests[0].parent.rename(kit)
        manifest = json.loads((kit / 'shin-kit.json').read_text())
        expected = {'shin-kit.json'}
        for item in manifest['files']:
            relative = item['path']
            assert relative not in expected
            expected.add(relative)
            path = kit / relative
            data = path.read_bytes()
            assert len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256'], relative
        actual = {p.relative_to(kit).as_posix() for p in kit.rglob('*') if p.is_file()}
        assert actual == expected, (actual - expected, expected - actual)
        for name in ('shin-ce.exe', 'shin-context.exe', 'shin-observer.exe', 'shin-mcp.exe', 'shin-tray.exe',
                     'vcruntime140.dll', 'msvcp140.dll'):
            assert (kit / 'bin' / name).is_file(), name
        ce = kit / 'bin/shin-ce.exe'
        assert json.loads(run([ce, '--version']))['version'] == manifest['version']
        assert json.loads(run([kit / 'bin/shin-context.exe', '--version']))['version'] == manifest['core_version']
        assert (kit / 'share/shin-observer/licenses/imgui/LICENSE.txt').is_file()
        run([kit / 'bin/shin-tray.exe', '--render-test', base / 'renders'])
        rendered = json.loads((base / 'renders/result.json').read_text())
        assert rendered['frames'] == 30 and rendered['cold_frames'] == 15
        assert rendered['documentation_frames'] == 20
        root = base / 'project'
        root.mkdir()
        (root / 'quota.cpp').write_text('int quotaLimit() { return 42; }\n', encoding='utf-8')
        (root / 'README.md').write_text('# Quota\nquotaLimit owns the project quota.\n', encoding='utf-8')
        home = base / 'home'
        try:
            ready = json.loads(run([ce, 'init', '--root', root, '--home', home]))
            assert ready['local_ready'] and ready['service_enrolled']
            answer = json.loads(run([ce, 'query', 'quotaLimit', '--root', root, '--home', home]))
            assert answer['ok'] and 'quotaLimit' in answer['source_context']
        finally:
            subprocess.run([str(ce), 'service-stop', '--home', str(home)], capture_output=True, timeout=65)
            deadline = time.monotonic() + 15
            wake = threading.Event()
            while time.monotonic() < deadline:
                probe = subprocess.run([str(ce), 'service-status', '--home', str(home)], capture_output=True, timeout=5)
                if probe.returncode:
                    break
                wake.wait(.05)
            else:
                raise AssertionError('Owned fixture service did not stop')
        assert (root / 'quota.cpp').read_text() == 'int quotaLimit() { return 42; }\n'
        print('Windows ZIP: hashes, DLLs, relocation, native Monitor renders, init/query/stop passed')


if __name__ == '__main__':
    main()
