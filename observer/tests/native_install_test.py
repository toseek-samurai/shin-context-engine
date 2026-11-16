"""Check real install rules, relocation and native identities without enrollment."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd, env):
    result = subprocess.run(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--config', default='')
    parser.add_argument('--observer', required=True)
    parser.add_argument('--ce', required=True)
    parser.add_argument('--mcp')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='shin-native-install-') as temporary:
        base = Path(temporary).resolve()
        home = base / 'home'
        home.mkdir()
        env = {**os.environ, 'HOME': str(home), 'USERPROFILE': str(home)}
        # A user's packaging environment must not redirect this disposable install.
        env.pop('DESTDIR', None)
        staged = base / 'stage'
        command = [args.cmake, '--install', args.build, '--prefix', str(staged)]
        if args.config:
            command += ['--config', args.config]
        run(command, base, env)
        relocated = base / 'relocated kit'
        staged.rename(relocated)
        suffix = '.exe' if os.name == 'nt' else ''
        for name, reference in [('shin-observer', args.observer), ('shin-ce', args.ce)]:
            installed = relocated / 'bin' / (name + suffix)
            assert installed.is_file(), f'product install omits {name}'
            original = json.loads(run([reference, '--version'], base, env))
            assert json.loads(run([str(installed), '--version'], base, env)) == original
        share = relocated / 'share' / 'shin-observer'
        assert (share / 'VERSION').read_text().strip() == original['version']
        assert not (share / 'licenses/cpp-httplib').exists(), 'removed Server transport was packaged'
        capabilities = json.loads(run([str(relocated / 'bin' / ('shin-observer' + suffix)), '--version'], base, env))['result']
        assert capabilities['synchronization_available'] is False
        assert capabilities['delivery_scope'] == 'local-only'
        # Retired commands/options fail before reading config or creating state.
        observer = str(relocated / 'bin' / ('shin-observer' + suffix))
        config = str(base / 'absent-config.json')
        for arguments in (['deliver', '--config', config], ['reconcile', '--config', config],
                          ['sample', '--config', config, '--delivery-config', config],
                          ['watch', '--config', config, '--delivery-config', config]):
            refused = subprocess.run([observer, *arguments], cwd=base, env=env,
                                     stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
            assert refused.returncode == 1 and not refused.stdout, arguments
            error = json.loads(refused.stderr)
            assert error['ok'] is False and error['error']['code'] == 'invalid-arguments', error
        for name in ('nlohmann/LICENSE.MIT', 'simdjson/LICENSE-APACHE-2.0',
                     'simdjson/LICENSE-MIT', 'tree-sitter/LICENSE', 'tree-sitter-unicode/LICENSE',
                     'tree-sitter-java/LICENSE', 'tree-sitter-typescript/LICENSE', 'tree-sitter-cpp/LICENSE',
                     'pugixml/LICENSE.MIT', 'libuv/LICENSE', 'libuv/LICENSE-extra'):
            assert (share / 'licenses' / name).stat().st_size > 0, name
        if os.name != 'nt':
            assert (share / 'licenses/ftxui/LICENSE').stat().st_size > 0
        if args.mcp:
            assert (relocated / 'bin' / ('shin-mcp' + suffix)).stat().st_size > 0
            assert (share / 'licenses/mcp-cpp-sdk/LICENSE').stat().st_size > 0
        profile = relocated / 'share/shin-context/profiles/generic-source.json'
        assert json.loads(profile.read_text())['id'] == 'generic-source'
        assert not any(home.iterdir()), 'version inspection wrote user state'
        print('native product install/relocation/version/licenses passed')


if __name__ == '__main__':
    main()
