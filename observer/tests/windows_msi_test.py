"""Explicit elevated MSI phase driver; real user-logon proof is separate."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import winreg

from windows_kit_test import check_msi, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('msi', type=Path)
    parser.add_argument('--phase', required=True, choices=('metadata', 'install', 'repair', 'uninstall'))
    args = parser.parse_args()
    assert os.name == 'nt'
    msi = args.msi.resolve()
    properties = check_msi(msi)
    if args.phase == 'metadata':
        return
    assert ctypes.windll.shell32.IsUserAnAdmin(), 'Run this explicit installer phase from an elevated terminal'
    api = ctypes.WinDLL('msi', use_last_error=True)
    api.MsiQueryProductStateW.argtypes = [ctypes.c_wchar_p]
    api.MsiQueryProductStateW.restype = ctypes.c_int
    api.MsiEnumRelatedProductsW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_wchar_p]

    def remembered_root():
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'Software\Shin\DesktopPreview',
                                0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                return Path(winreg.QueryValueEx(key, 'InstallRoot')[0]).resolve()
        except FileNotFoundError:
            return None

    present = api.MsiQueryProductStateW(properties['ProductCode']) == 5
    if args.phase == 'install':
        existing = ctypes.create_unicode_buffer(39)
        assert api.MsiEnumRelatedProductsW(properties['UpgradeCode'], 0, 0, existing) == 259, 'Existing preview must not be adopted by this test'
        assert not present and remembered_root() is None
    else:
        assert present and remembered_root() is not None, 'Only this exact MSI product may be maintained'
    previous_root = remembered_root()
    home = Path(os.environ['USERPROFILE']) / '.shin-ce'
    previous_home = home.stat().st_ino if home.exists() else None
    descriptor, log = tempfile.mkstemp(prefix='shin-msi-phase-', suffix='.log')
    os.close(descriptor)
    switch = {'install': '/i', 'repair': '/fa', 'uninstall': '/x'}[args.phase]
    result = subprocess.run(['msiexec.exe', switch, str(msi), '/qn', '/norestart', '/L*V', log],
                            capture_output=True, timeout=240)
    assert result.returncode in (0, 3010), (result.returncode, log)
    if args.phase == 'uninstall':
        assert api.MsiQueryProductStateW(properties['ProductCode']) != 5
        assert remembered_root() is None
        assert not (previous_root / 'bin/shin-ce.exe').exists()
    else:
        root = remembered_root()
        assert root is not None and api.MsiQueryProductStateW(properties['ProductCode']) == 5
        if previous_root:
            assert root == previous_root
        assert json.loads(run([root / 'bin/shin-ce.exe', '--version']))['tool'] == 'shin-ce'
    if previous_home is not None:
        assert home.is_dir() and home.stat().st_ino == previous_home, 'Installer replaced the user home'
    Path(log).unlink()
    print(f'MSI {args.phase} completed; no project was enrolled. User-logon/recovery proof remains separate.')


if __name__ == '__main__':
    main()
