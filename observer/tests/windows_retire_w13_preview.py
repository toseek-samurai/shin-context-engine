"""One-time, elevated EVO-X1 lab cleanup of the exact installed W13 preview.

Not shipped in the product. Refuses other images/versions; preserves all user
data. Retires the stuck template and invokes Windows Installer for this product.
"""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import os
from pathlib import Path
import subprocess
import threading
import time
import winreg


def main():
    assert C.windll.shell32.IsUserAnAdmin(), 'Run from an Administrator PowerShell'
    root = Path(os.environ['ProgramFiles']) / 'Shin Desktop Preview'
    tool = root / 'bin/shin-ce.exe'
    expected = '0e04b4dffe9144dc102dd681f98515650854373ba224787da2f70ff78088ad23'
    assert hashlib.sha256(tool.read_bytes()).hexdigest() == expected, 'Not the qualified W13 image; no mutation'
    products = []
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                       0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as uninstall:
        for i in range(winreg.QueryInfoKey(uninstall)[0]):
            key = winreg.EnumKey(uninstall, i)
            with winreg.OpenKey(uninstall, key) as item:
                try:
                    if (winreg.QueryValueEx(item, 'DisplayName')[0] == 'Shin Desktop Preview'
                            and winreg.QueryValueEx(item, 'Publisher')[0] == 'Shin Context Engine'
                            and winreg.QueryValueEx(item, 'WindowsInstaller')[0] == 1):
                        products.append(key)
                except FileNotFoundError:
                    pass
    assert len(products) == 1 and len(products[0]) == 38 and products[0].startswith('{'), products
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'Software\Shin\DesktopPreview',
                       0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        assert Path(winreg.QueryValueEx(key, 'InstallRoot')[0]).resolve() == root.resolve()

    api, kernel = C.WinDLL('advapi32', use_last_error=True), C.WinDLL('kernel32', use_last_error=True)
    api.OpenSCManagerW.argtypes = [W.LPCWSTR, W.LPCWSTR, W.DWORD]; api.OpenSCManagerW.restype = W.HANDLE
    api.OpenServiceW.argtypes = [W.HANDLE, W.LPCWSTR, W.DWORD]; api.OpenServiceW.restype = W.HANDLE
    api.CloseServiceHandle.argtypes = [W.HANDLE]
    api.QueryServiceStatusEx.argtypes = [W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; kernel.OpenProcess.restype = W.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
    kernel.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
    kernel.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
    kernel.CloseHandle.argtypes = [W.HANDLE]

    class Status(C.Structure):
        _fields_ = [(key, W.DWORD) for key in ('type', 'state', 'controls', 'exit', 'specific', 'checkpoint', 'hint', 'pid', 'flags')]

    def status(handle):
        value, size = Status(), W.DWORD()
        assert api.QueryServiceStatusEx(handle, 0, C.byref(value), C.sizeof(value), C.byref(size))
        return value

    names = []
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Services') as services:
        for i in range(winreg.QueryInfoKey(services)[0]):
            key = winreg.EnumKey(services, i)
            if key == 'ShinObserver' or key.startswith('ShinObserver_'):
                with winreg.OpenKey(services, key) as item:
                    assert winreg.QueryValueEx(item, 'ImagePath')[0] == f'"{tool}" _scm-service'
                    assert winreg.QueryValueEx(item, 'Type')[0] == (0x50 if key == 'ShinObserver' else 0xd0)
                names.append(key)
    assert 'ShinObserver' in names, 'No W13 template; do not use this lab transition again'
    scm = api.OpenSCManagerW(None, None, 1)
    assert scm
    try:
        # Disable the exact template before retiring its existing instances.
        for name in sorted(names):
            subprocess.run(['sc.exe', 'config', name, 'start=', 'disabled'], check=True, capture_output=True)
        for name in names:
            if name == 'ShinObserver':
                continue
            service = api.OpenServiceW(scm, name, 4)
            assert service
            try:
                current = status(service)
                if current.state == 2 and current.pid and not current.controls & 1:
                    process = kernel.OpenProcess(0x100001 | 0x1000, False, current.pid)
                    assert process
                    try:
                        image, size = C.create_unicode_buffer(32768), W.DWORD(32768)
                        assert kernel.QueryFullProcessImageNameW(process, 0, image, C.byref(size))
                        assert Path(image.value).resolve() == tool.resolve() and status(service).pid == current.pid
                        assert kernel.TerminateProcess(process, 1)
                        assert kernel.WaitForSingleObject(process, 10000) == 0
                    finally:
                        kernel.CloseHandle(process)
                elif current.state != 1:
                    subprocess.run(['sc.exe', 'stop', name], check=True, capture_output=True)
                deadline = time.monotonic() + 15
                wake = threading.Event()
                while status(service).state != 1 and time.monotonic() < deadline:
                    wake.wait(.1)
                assert status(service).state == 1, 'Old instance has not stopped'
            finally:
                api.CloseServiceHandle(service)
        subprocess.run([str(tool), '_scm-uninstall'], check=True, timeout=90)
    finally:
        api.CloseServiceHandle(scm)
    log = Path(os.environ['TEMP']) / 'shin-w13-remove.log'
    result = subprocess.run(['msiexec.exe', '/x', products[0], '/passive', '/norestart', '/L*V', str(log)], timeout=240)
    assert result.returncode == 0, (result.returncode, str(log))
    assert not tool.exists(), 'Old installed payload remains'
    print('Exact W13 preview removed. User data was not touched. Install the dev.2 MSI normally.')


if __name__ == '__main__':
    main()
