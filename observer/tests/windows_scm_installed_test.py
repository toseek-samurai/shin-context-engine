"""Explicit real SCM-host runtime gate; first install must need no new logon."""
import argparse
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ce', type=Path)
    parser.add_argument('--allow-test-enrollment', action='store_true', required=True)
    args = parser.parse_args()
    tool = args.ce.resolve()
    assert not ctypes.windll.shell32.IsUserAnAdmin(), 'Run the user runtime gate without elevation'
    wake = threading.Event()

    def ce(*options, okay=True):
        result = subprocess.run([str(tool), *map(str, options)], capture_output=True, encoding='utf-8', timeout=75)
        value = json.loads(result.stdout or result.stderr)
        if okay:
            assert result.returncode == 0 and value['ok'], value
        return value

    def wait(probe, label, seconds=60):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            value = probe()
            if value:
                return value
            wake.wait(.1)
        raise AssertionError(label)

    installed = ce('service-installation')
    assert installed['manager'] == 'windows-scm-host' and installed['host_running'], installed
    assert installed.get('instance_present'), installed
    assert installed['process_id'] != installed['host_process_id']
    # The installed child's token, not a reported JSON label, proves it is
    # unelevated and belongs to this exact OS user.
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    security = ctypes.WinDLL('advapi32', use_last_error=True)
    api.OpenProcess.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint]
    api.OpenProcess.restype = ctypes.c_void_p
    api.GetCurrentProcess.restype = ctypes.c_void_p
    api.CloseHandle.argtypes = [ctypes.c_void_p]
    security.OpenProcessToken.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
    security.GetTokenInformation.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
    security.EqualSid.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    process = api.OpenProcess(0x1000, False, installed['process_id'])
    assert process
    handles = []
    try:
        identities = []
        for owner in (api.GetCurrentProcess(), process):
            token = ctypes.c_void_p()
            assert security.OpenProcessToken(owner, 8, ctypes.byref(token))
            handles.append(token)
            size = ctypes.c_uint()
            security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
            assert 0 < size.value < 65536
            identity = ctypes.create_string_buffer(size.value)
            assert security.GetTokenInformation(token, 1, identity, size.value, ctypes.byref(size))
            identities.append(identity)
            elevated = ctypes.c_uint()
            assert security.GetTokenInformation(token, 20, ctypes.byref(elevated), 4, ctypes.byref(size))
            assert elevated.value == 0
        assert security.EqualSid(*(ctypes.cast(item, ctypes.POINTER(ctypes.c_void_p))[0] for item in identities))
    finally:
        for token in handles:
            api.CloseHandle(token)
        api.CloseHandle(process)
    direct = ce('_scm-user', 'Global\\ShinObserverStop-' + '0' * 32, okay=False)
    assert direct['error']['code'] == 'service-managed-only', direct
    home = Path(installed['service_home'])
    registry = home / 'registry/state.json'
    wait(lambda: registry.is_file(), 'SCM worker private registry bootstrap')
    assert json.loads(registry.read_text(encoding='utf-8'))['projects'] == [], 'Do not interrupt an enrolled real project'
    ce('service-start')
    root = Path(tempfile.mkdtemp(prefix='shin-scm-runtime-')).resolve()
    (root / 'quota.cpp').write_text('int quotaLimit() { return 37; }\n', encoding='utf-8')
    projects = home / 'projects'
    before = set(projects.iterdir()) if projects.exists() else set()
    enrolled = False
    try:
        ready = ce('init', '--root', root)
        enrolled = True
        assert ready['local_ready'] and ready['service']['supervision'] == 'windows-scm-host', ready
        identity = ce('identity', '--root', root)['identity']
        assert 'quotaLimit' in ce('query', 'quotaLimit', '--root', root)['source_context']
        ce('service-stop')
        wait(lambda: ce('service-installation')['worker_state'] == 'stopped', 'User runtime normal stop')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = ce('service-installation')
            assert status['host_running'] and status['worker_state'] == 'stopped', 'Normal stop restarted or stopped the host'
            wake.wait(.25)
        ce('service-start')
        assert ce('identity', '--root', root)['identity'] == identity
        service = ce('service-installation')
        assert service['service_running'] and service['process_id']
        api = ctypes.WinDLL('kernel32', use_last_error=True)
        api.OpenProcess.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint]
        api.OpenProcess.restype = ctypes.c_void_p
        api.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint)]
        api.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        process = api.OpenProcess(0x1001, False, service['process_id'])
        assert process, ctypes.get_last_error()
        try:
            image = ctypes.create_unicode_buffer(32768); size = ctypes.c_uint(len(image))
            assert api.QueryFullProcessImageNameW(process, 0, image, ctypes.byref(size))
            assert Path(image.value).resolve() == tool
            assert len(json.loads(registry.read_text(encoding='utf-8'))['projects']) == 1
            assert api.TerminateProcess(process, 0xC0000005)
        finally:
            api.CloseHandle(process)
        def recovered():
            current = ce('service-installation')
            return current['service_running'] and current['process_id'] != service['process_id']
        wait(recovered, 'SCM failed to recover the real Observer', 60)
        wait(lambda: ce('query', 'quotaLimit', '--root', root, okay=False).get('ok'), 'Query after SCM recovery')
        assert ce('identity', '--root', root)['identity'] == identity
        print('Installed SCM host: user process, query, normal stop, crash recovery and identity passed', flush=True)
    finally:
        if enrolled:
            ce('service-start')
            ce('detach', '--root', root)
        assert json.loads(registry.read_text(encoding='utf-8'))['projects'] == []
        # Only the freshly created, detached fixture workspace is disposable.
        for workspace in (set(projects.iterdir()) if projects.exists() else set()) - before:
            binding = json.loads((workspace / 'state.json').read_text(encoding='utf-8'))
            lifecycle = json.loads((workspace / 'lifecycle/state.json').read_text(encoding='utf-8'))
            assert Path(binding['root']).resolve() == root and lifecycle['phase'] == 'detached'
            shutil.rmtree(workspace)
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
