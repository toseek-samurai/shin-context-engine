#!/usr/bin/env python3
"""Mock only HTTPS transport; execute the real release bootstrap and native kit."""
import argparse
import json
import os
import shlex
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('script', type=Path)
p.add_argument('archive', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='.shin-bootstrap-', dir=Path.home()) as directory:
    root = Path(directory).resolve()
    user = root / "user's space $not_a_variable"
    user.mkdir(mode=0o700)
    tools = root / 'transport'
    tools.mkdir()
    fake = tools / 'curl'
    fake.write_text('''#!/bin/sh
set -eu
url= output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        https://*) url=$1; shift ;;
        --output) output=$2; shift 2 ;;
        *) shift ;;
    esac
done
case "$url" in
    https://github.com/toseek-samurai/shin-context-engine/releases/download/v*/*.zip.sha256)
        cp "$FIXTURE_CHECKSUM" "$output" ;;
    https://github.com/toseek-samurai/shin-context-engine/releases/download/v*/*.zip)
        cp "$FIXTURE_ARCHIVE" "$output" ;;
    *) exit 97 ;;
esac
''')
    fake.chmod(0o755)
    bad = root / 'bad.sha256'
    bad.write_text('0' * 64 + '  archive.zip\n')
    env = {**os.environ, 'HOME': str(user), 'TMPDIR': str(root),
           'PATH': str(tools) + os.pathsep + os.environ['PATH'],
           'FIXTURE_ARCHIVE': str(a.archive.resolve()), 'FIXTURE_CHECKSUM': str(bad)}

    def run(*args, ok=True):
        result = subprocess.run(['/bin/sh', str(a.script.resolve()), *map(str, args)],
                                env=env, text=True, capture_output=True, timeout=90)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        return result

    run('--unknown', ok=False)
    run(ok=False)
    assert not (user / '.local/opt/shin-local').exists()
    env['FIXTURE_CHECKSUM'] = str(Path(str(a.archive.resolve()) + '.sha256'))
    first = run()
    assert '"phase": "installed"' in first.stdout
    launch = next(line.strip() for line in first.stdout.splitlines() if line.startswith('  open '))
    assert shlex.split(launch) == ['open', str(user / '.local/opt/shin-local/Shin Monitor.app')]
    commands = [shlex.split(line.strip()) for line in first.stdout.splitlines() if line.startswith("  '")]
    assert commands[0][0] == str(user / '.local/opt/shin-local/bin/shin-ce')
    assert commands[1][1] == 'client-targets'
    again = run()
    assert '"changed": false' in again.stdout
    custom = user / 'custom kit'
    data, commands = user / 'custom data', user / 'custom commands'
    run('--prefix', custom, '--home', data, '--bin-dir', commands)
    assert '"changed": false' in run('--prefix', custom).stdout
    recorded = json.loads((custom / '.shin-installation.json').read_text())
    assert recorded['home'] == str(data) and recorded['bin_dir'] == str(commands)
    subprocess.run([str(custom / 'bin/shin-ce'), 'uninstall', '--prefix', str(custom)],
                   env=env, check=True, capture_output=True, timeout=30)
    prefix = user / '.local/opt/shin-local'
    # Legacy installs must be explicitly adopted, never inferred from a directory.
    alien = user / 'foreign kit'
    alien.mkdir()
    (alien / 'note').write_text('preserve')
    run('--prefix', alien, ok=False)
    assert (alien / 'note').read_text() == 'preserve'
    assert not (alien / '.shin-installation.json').exists()
    subprocess.run([str(prefix / 'bin/shin-ce'), 'uninstall'], env=env, check=True,
                   capture_output=True, timeout=30)
    assert not prefix.exists()
    assert not list(root.glob('shin-install.*')), 'bootstrap leaked temporary downloads'
    print(json.dumps({'ok': True, 'checksum_refusal': True, 'install_and_repeat_update': True,
                      'spaces_in_paths': True, 'custom_paths_preserved': True, 'implicit_adoption_refused': True,
                      'transport': 'fixture', 'native_installer': 'real archive'}))
