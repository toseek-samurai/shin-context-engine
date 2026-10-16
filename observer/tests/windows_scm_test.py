"""Unprivileged SCM contract checks; not a substitute for installed recovery proof."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


tool = Path(sys.argv[1]).resolve()


def run(*args):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, encoding='utf-8', timeout=15)
    return result.returncode, json.loads(result.stdout or result.stderr)


with tempfile.TemporaryDirectory(prefix='shin-scm-contract-') as directory:
    home = Path(directory) / 'uncreated home'
    code, definition = run('service-definition', '--home', home)
    assert code == 0 and definition['ok'], definition
    assert definition['manager'] == 'windows-scm-host'
    assert definition['service_type'] == 'SERVICE_WIN32_OWN_PROCESS'
    assert definition['runtime_account'] == 'logged-on-user-unelevated'
    assert definition['host_account'] == 'LocalSystem'
    assert definition['password_stored'] is False
    assert definition['recovery_owner'] == 'windows-scm-and-owned-jobs'
    assert definition['normal_stop_restarts'] is False
    assert definition['restart_attempts'] == 3
    assert definition['arguments'] == ['_scm-host']
    assert definition['installation_performed'] is False
    assert not home.exists()
    code, refused = run('_scm-host')
    assert code == 2 and refused['error']['code'] == 'service-manager-failed', refused
    code, refused = run('_scm-user', 'Global\\ShinObserverStop-' + '0' * 32)
    assert code == 2 and refused['error']['code'] in ('unsafe-service-executable', 'service-managed-only'), refused
    for command in ('_scm-install', '_scm-uninstall', '_scm-stop'):
        code, refused = run(command)
        assert code == 2 and refused['error']['code'] == 'unsafe-service-executable', refused
    code, refused = run('service-install', '--home', home)
    assert code == 2 and refused['error']['code'] == 'service-home-conflict', refused
    assert not home.exists()
print('SCM definition, direct-entry refusal, writable-kit refusal and no state mutation passed')
