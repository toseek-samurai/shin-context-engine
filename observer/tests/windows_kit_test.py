"""Validate the actual Windows archive; no real user enrollment or MSI install."""
import argparse
import ctypes
import contextlib
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import zipfile
import uuid


def check_msi_version(rows, properties):
    """Verify compiled MSI identity and the actual upgrade/downgrade bounds."""
    source = properties['SHIN_PREVIEW_VERSION']
    match = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(dev|rc)\.(0|[1-9][0-9]*))?', source)
    assert match, source
    major, minor, patch = map(int, match.group(1, 2, 3))
    channel, serial = match.group(4, 5)
    assert major <= 255 and minor <= 255 and patch <= 64, source
    offset = 999
    if channel:
        serial = int(serial)
        assert serial <= (899 if channel == 'dev' else 98), source
        offset = serial if channel == 'dev' else 900 + serial
    version = f'{major}.{minor}.{patch * 1000 + offset}'
    assert properties['ProductVersion'] == version, properties['ProductVersion']
    namespace = uuid.UUID('4D5D4627-1B39-4F4A-ABEE-17CE51649F28')
    assert uuid.UUID(properties['UpgradeCode']) == namespace
    assert uuid.UUID(properties['ProductCode']) == uuid.uuid5(namespace, f'windows-x64:{source}')
    upgrades = rows('SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `Attributes`, `ActionProperty` FROM `Upgrade`', 5)
    older = [row for row in upgrades if row[4] == 'WIX_UPGRADE_DETECTED']
    newer = [row for row in upgrades if row[4] == 'WIX_DOWNGRADE_DETECTED']
    assert len(older) == len(newer) == 1, upgrades
    assert all(uuid.UUID(row[0]) == namespace for row in (older[0], newer[0]))
    assert older[0][1:3] == ['', version] and not int(older[0][3]) & (2 | 512), older
    assert newer[0][1:3] == [version, ''] and int(newer[0][3]) & 2, newer
    conditions = rows('SELECT `Condition` FROM `LaunchCondition`', 1)
    assert any('NOT WIX_DOWNGRADE_DETECTED' in row[0] for row in conditions), conditions


def check_msi_ui(rows, properties):
    """Check the compiled wizard, including maintenance and the acceptance gate."""
    styles = rows('SELECT `TextStyle`, `FaceName`, `Size` FROM `TextStyle`', 3)
    assert ['WixUI_Font_Normal', 'Segoe UI', '9'] in styles, styles
    assert properties['SHIN_PREVIEW_VERSION']
    assert properties['ARPPRODUCTICON'] == 'ProductIcon.ico'
    assert ['ProductIcon.ico'] in rows('SELECT `Name` FROM `Icon`', 1)
    controls = {(row[0], row[1]): row[2:] for row in rows(
        'SELECT `Dialog_`, `Control`, `Type`, `Height`, `Text` FROM `Control`', 5)}
    assert 'Welcome to Shin' in controls['WelcomeDlg', 'Title'][2]
    assert int(controls['WelcomeDlg', 'Description'][1]) >= 130
    assert 'SHIN_PREVIEW_VERSION' in controls['WelcomeDlg', 'Description'][2]
    assert '[INSTALL_ROOT]' in controls['VerifyReadyDlg', 'InstallText'][2]
    assert int(controls['VerifyReadyDlg', 'InstallText'][1]) >= 136
    notice = controls['LicenseAgreementDlg', 'LicenseText'][2]
    assert r'\f0 Segoe UI;' in notice and r'\fs18' in notice, notice[:100]
    assert 'Apache-2.0' in notice and 'preserved during removal' in notice
    assert not any(dialog == 'InstallDirDlg' for dialog, _ in controls), 'Protected install root must not become editable'
    sequence = dict(rows('SELECT `Action`, `Sequence` FROM `InstallUISequence`', 2))
    assert 'WelcomeEulaDlg' not in sequence
    assert int(sequence['WelcomeDlg']) < int(sequence['ProgressDlg'])
    events = rows('SELECT `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`, `Ordering` FROM `ControlEvent`', 6)
    for dialog, control, target, condition in (
            ('WelcomeDlg', 'Next', 'LicenseAgreementDlg', 'NOT Installed'),
            ('WelcomeDlg', 'Next', 'VerifyReadyDlg', 'Installed AND PATCH'),
            ('LicenseAgreementDlg', 'Back', 'WelcomeDlg', '1'),
            ('LicenseAgreementDlg', 'Next', 'VerifyReadyDlg', 'LicenseAccepted = "1"'),
            ('VerifyReadyDlg', 'Back', 'LicenseAgreementDlg', 'NOT Installed'),
            ('VerifyReadyDlg', 'Back', 'MaintenanceTypeDlg', 'Installed AND NOT PATCH'),
            ('VerifyReadyDlg', 'Back', 'WelcomeDlg', 'Installed AND PATCH'),
            ('MaintenanceWelcomeDlg', 'Next', 'MaintenanceTypeDlg', '1'),
            ('MaintenanceTypeDlg', 'RepairButton', 'VerifyReadyDlg', '1'),
            ('MaintenanceTypeDlg', 'RemoveButton', 'VerifyReadyDlg', '1'),
            ('MaintenanceTypeDlg', 'Back', 'MaintenanceWelcomeDlg', '1')):
        assert any(row[:5] == [dialog, control, 'NewDialog', target, condition] for row in events), (dialog, control, target)
    gate = rows("SELECT `Action`, `Condition` FROM `ControlCondition` WHERE `Dialog_` = 'LicenseAgreementDlg' AND `Control_` = 'Next'", 2)
    assert ['Enable', 'LicenseAccepted = "1"'] in gate
    assert ['Disable', 'LicenseAccepted <> "1"'] in gate
    finish = [row for row in events if row[:2] == ['ExitDialog', 'Finish']]
    launch = next(row for row in finish if row[2:4] == ['DoAction', 'ShinLaunchMonitor'])
    end = next(row for row in finish if row[2:4] == ['EndDialog', 'Return'])
    assert int(launch[5]) < int(end[5]), finish
    print('MSI UI: branding, readable notes, protected root, install/maintenance navigation and acceptance gate passed')


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
        check_msi_version(rows, properties)
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
        assert 'NOT REMOVE~="ALL"' in launch[0][4]
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
        check_msi_ui(rows, properties)
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
    parser.add_argument('--headless-render', action='store_true',
                        help='Render without Explorer; the default also requires real notification-area checks')
    parser.add_argument('--keep', action='store_true', help='Retain this disposable fixture for failure diagnosis')
    args = parser.parse_args()
    assert os.name == 'nt', 'Run against native Windows binaries'
    msi_properties = check_msi(args.msi) if args.msi else None
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
        if msi_properties:
            assert manifest['version'] == msi_properties['SHIN_PREVIEW_VERSION'], 'ZIP and MSI versions differ'
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
        run([kit / 'bin/shin-tray.exe', '--render-headless-test' if args.headless_render else '--render-test', base / 'renders'])
        rendered = json.loads((base / 'renders/result.json').read_text())
        assert rendered['frames'] == 30 and rendered['cold_frames'] == 15
        assert rendered['documentation_frames'] == 20
        assert rendered['shell_integration'] is (not args.headless_render)
        assert rendered['taskbar_recreated_event'] is (None if args.headless_render else True)
        assert rendered['focus_dismissal'] and rendered['escape']
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
