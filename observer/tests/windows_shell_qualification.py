#!/usr/bin/env python3
"""Qualify the native shell using an owned copy, independent of workspace ACLs."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def private(path, sid, directory=False):
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    convert.restype = ctypes.c_int
    apply = advapi.SetFileSecurityW
    apply.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_void_p]
    apply.restype = ctypes.c_int
    descriptor = ctypes.c_void_p()
    flags = 'OICI' if directory else ''
    sddl = f'O:{sid}D:P(A;{flags};FA;;;{sid})'
    if not convert(sddl, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not apply(str(path), 0x80000005, descriptor):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        free = ctypes.WinDLL('kernel32').LocalFree
        free.argtypes = [ctypes.c_void_p]
        free(descriptor)


def main():
    if os.name != 'nt' or len(sys.argv) != 3:
        raise SystemExit('Use on Windows: windows_shell_qualification.py TRAY_EXE OUTPUT')
    executable, output = (Path(value).resolve() for value in sys.argv[1:])
    identity = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'],
                                       encoding='utf-8', errors='replace')
    sid = re.search(r'S-1-5-[0-9-]+', identity).group()
    artifacts = ('result.json', 'desktop-en.png', 'desktop-ru.png', 'quick-en.png', 'quick-ru.png')
    output.mkdir(parents=True, exist_ok=True)
    for name in artifacts:
        (output / name).unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='shin-windows-shell-') as temporary:
        base = Path(temporary)
        private(base, sid, directory=True)
        binary = base / executable.name
        libraries = list(executable.parent.glob('*.dll'))
        if len(libraries) > 16:
            raise RuntimeError('Shell fixture has too many runtime libraries')
        for source in [executable, *libraries]:
            if source.stat().st_size > 128 * 1024 * 1024:
                raise RuntimeError('Shell fixture runtime file exceeds its bound')
            target = base / source.name
            shutil.copyfile(source, target)
            private(target, sid)
            if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
                raise RuntimeError('Shell fixture runtime changed during copying')
        fixture = base / 'output'
        result = subprocess.run([str(binary), '--window-shell-test', str(fixture)],
                                capture_output=True, encoding='utf-8', errors='replace', timeout=25)
        print(result.stdout, end='')
        print(result.stderr, end='', file=sys.stderr)
        for name in artifacts:
            if (fixture / name).is_file():
                shutil.copyfile(fixture / name, output / name)
        if result.returncode:
            raise SystemExit(result.returncode)
        receipt = json.loads((fixture / 'result.json').read_text(encoding='utf-8'))
        if not receipt.get('ok') or not receipt.get('source_fixture_only') or not {
                'second_process_reuses_existing_desktop', 'qualification_lock_collision_fails_closed'
        }.issubset(receipt.get('checks', [])):
            raise RuntimeError('Native shell qualification did not complete its process/ownership checks')
        print(f'Native shell: {len(receipt["checks"])} checks passed using an owned, byte-identical runtime copy.')


if __name__ == '__main__':
    main()
