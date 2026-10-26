#!/usr/bin/env python3
"""Publish only the exact macOS artifact from the matching, successful CI matrix."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import zipfile

MATRIX = {f'native ({system}, {component})'
          for system in ('ubuntu-24.04', 'macos-15') for component in ('core', 'observer')}
VERSION = r'(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})-dev\.(?:0|[1-9][0-9]{0,8})'


def require(condition, message):
    if not condition:
        raise ValueError(message)


class PackageProofPending(ValueError):
    """Completed-job step metadata has not reached the API yet."""


def qualification(run, jobs, artifact, repo, sha, run_id, attempt, recover_publisher=False):
    require(run['head_sha'] == sha and run['id'] == run_id and run['run_attempt'] == attempt,
            'CI identity or attempt differs from the requested source')
    require(run['repository']['full_name'] == repo and run['head_repository']['full_name'] == repo
            and run['event'] == 'push' and run['path'] == '.github/workflows/ci.yml', 'Wrong CI provenance')
    publisher = [j for j in jobs if j['name'] == 'publish-preview']
    recovered = (recover_publisher and run['status'] == 'completed' and run['conclusion'] == 'failure'
                 and len(publisher) == 1 and publisher[0]['status'] == 'completed'
                 and publisher[0]['conclusion'] == 'failure'
                 and len(jobs) == len(MATRIX) + 1)
    require((run['conclusion'] in (None, 'success') or recovered)
            and run['status'] in ('in_progress', 'completed'),
            'CI run is not eligible for publication')
    # The publisher itself is still running; every native matrix job must finish.
    for name in MATRIX:
        matches = [job for job in jobs if job['name'] == name]
        require(len(matches) == 1 and matches[0]['status'] == 'completed'
                and matches[0]['conclusion'] == 'success', f'Unqualified matrix job: {name}')
    mac = next(job for job in jobs if job['name'] == 'native (macos-15, observer)')
    for step in ('Exact macOS kit qualification', 'Upload qualified macOS kit'):
        matches = [row for row in mac['steps'] if row['name'] == step]
        if not matches or (len(matches) == 1 and matches[0]['conclusion'] is None):
            raise PackageProofPending(f'Package proof metadata is pending: {step}')
        require(len(matches) == 1 and matches[0]['conclusion'] == 'success', f'Missing package proof: {step}')
    require(not artifact['expired'] and artifact['name'] == f'shin-local-macos-ARM64-{run_id}-{attempt}'
            and artifact['workflow_run']['id'] == run_id and artifact['workflow_run']['head_sha'] == sha,
            'Artifact does not belong to this qualified run')


def verify_assets(directory, version, core):
    name = f'shin-local-{version}-macos-arm64.zip'
    require({p.name for p in directory.iterdir()} == {name, name + '.sha256', 'install.sh', 'shin-updater-v1-macos-arm64.zip', 'shin-updater-v1-macos-arm64.zip.sha256'},
            'Expected the qualified kit, updater, both checksums and installer')
    archive = directory / name
    require(archive.stat().st_size <= 128 * 1024 * 1024, 'Archive exceeds the release bound')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    require((directory / (name + '.sha256')).read_text() in (f'{digest}  {name}', f'{digest}  {name}\n'),
            'Archive checksum mismatch')
    require(f"version='{version}'" in (directory / 'install.sh').read_text(), 'Installer version mismatch')
    with zipfile.ZipFile(archive) as kit:
        prefix = f'shin-local-{version}-macos-arm64/'
        require(all(n.startswith(prefix) and '..' not in Path(n).parts for n in kit.namelist()), 'Unsafe kit paths')
        manifest = json.loads(kit.read(prefix + 'shin-kit.json'))
        require(manifest['version'] == version and manifest['core_version'] == core, 'Kit component identity mismatch')
        require(0 < len(manifest['files']) <= 1024, 'Invalid kit inventory')
        for row in manifest['files']:
            info = kit.getinfo(prefix + row['path'])
            require(info.file_size <= 128 * 1024 * 1024, 'Kit file exceeds the release bound')
            value = kit.read(info)
            require(len(value) == row['size'] and hashlib.sha256(value).hexdigest() == row['sha256'],
                    'Kit manifest hash mismatch')
    updater_name = 'shin-updater-v1-macos-arm64.zip'
    updater = directory / updater_name
    updater_hash = hashlib.sha256(updater.read_bytes()).hexdigest()
    require(updater.stat().st_size <= 64 * 1024 * 1024 and
            (directory / (updater_name + '.sha256')).read_text() in
            (f'{updater_hash}  {updater_name}', f'{updater_hash}  {updater_name}\n'), 'Updater checksum mismatch')
    with zipfile.ZipFile(archive) as kit, zipfile.ZipFile(updater) as envelope:
        root = 'shin-updater-v1/'
        value = envelope.read(root + 'shin-updater.json')
        manifest = json.loads(value)
        require(manifest['schema_version'] == 'shin.updater.v1' and manifest['protocol'] == 1
                and manifest['version'] == version, 'Updater protocol/version mismatch')
        require(value == kit.read(prefix + 'shin-updater.json'), 'Updater differs from qualified kit')
        expected_paths = {root + 'shin-updater.json', *(root + row['path'] for row in manifest['files'])}
        directories = {'shin-updater-v1/'}
        for row in manifest['files']:
            directories.update(root + str(parent) + '/' for parent in Path(row['path']).parents if str(parent) != '.')
        expected_paths |= directories
        require(len(envelope.namelist()) == len(expected_paths) and set(envelope.namelist()) == expected_paths,
                'Unexpected updater inventory')
        require(root + 'bin/shin-updater' in expected_paths, 'Missing updater executable')
        for row in manifest['files']:
            require(not Path(row['path']).is_absolute() and '..' not in Path(row['path']).parts, 'Unsafe updater path')
            payload = envelope.read(root + row['path'])
            require(len(payload) == row['size'] and hashlib.sha256(payload).hexdigest() == row['sha256']
                    and payload == kit.read(prefix + row['path']), 'Updater payload differs from qualified kit')
    return {p.name: 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}


def release_matches(receipt, sha, expected, complete=True):
    require(receipt['targetCommitish'] == sha, 'Existing release targets different source')
    assets = receipt['assets']
    require(len({a['name'] for a in assets}) == len(assets), 'Duplicate release assets')
    for asset in assets:
        require(asset['name'] in expected and asset['state'] == 'uploaded'
                and asset['digest'] == expected[asset['name']], 'Existing release assets differ; no replacement allowed')
    if complete:
        require({a['name'] for a in assets} == set(expected), 'Release assets are incomplete')


def command(*args):
    result = subprocess.run(list(args), check=True, capture_output=True, text=True, timeout=180)
    require(len(result.stdout) <= 4 * 1024 * 1024, 'GitHub response exceeds the publication bound')
    return result.stdout


def publish(repo, sha, tag, run_id, attempt, source_root=None, recover_publisher=False):
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo), 'Invalid repository')
    require(re.fullmatch(r'[0-9a-f]{40}', sha) and re.fullmatch('v' + VERSION, tag), 'Invalid source or preview tag')
    require(run_id > 0 and attempt > 0, 'Invalid CI operation')
    root = Path(source_root).resolve() if source_root else Path(__file__).resolve().parents[1]
    version = tag[1:]
    require((root / 'observer/VERSION').read_text().strip() == version, 'Tag differs from Observer VERSION')
    require(command('git', '-C', str(root), 'rev-parse', 'HEAD').strip() == sha, 'Checkout differs from CI source')
    notes = (root / 'docs/releases' / (tag + '.md')).read_text()

    def api(path):
        return json.loads(command('gh', 'api', f'repos/{repo}/{path}'))

    def gate_once():
        run = api(f'actions/runs/{run_id}')
        page = api(f'actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100')
        require(page['total_count'] == len(page['jobs']), 'CI job list is incomplete')
        artifacts = api(f'actions/runs/{run_id}/artifacts?per_page=100')
        require(artifacts['total_count'] == len(artifacts['artifacts']), 'Artifact list is incomplete')
        selected = [a for a in artifacts['artifacts'] if a['name'] == f'shin-local-macos-ARM64-{run_id}-{attempt}']
        require(len(selected) == 1, 'Qualified artifact is missing or ambiguous')
        require(run['head_branch'] == tag, 'Qualification did not run for this preview tag')
        qualification(run, page['jobs'], selected[0], repo, sha, run_id, attempt, recover_publisher)
        require(api(f'commits/{tag}')['sha'] == sha, 'Release tag moved away from qualified source')
        return selected[0]['name']

    def gate():
        for retry in range(10):
            try:
                return gate_once()
            except PackageProofPending:
                if retry == 9:
                    raise
                time.sleep(3)  # Read-only metadata convergence; never retry mutations here.

    def receipt():
        result = subprocess.run(['gh', 'release', 'view', tag, '--repo', repo, '--json',
                                 'isDraft,isPrerelease,targetCommitish,assets,url'], capture_output=True, text=True, timeout=30)
        if result.returncode:
            require('release not found' in result.stderr.lower(), 'Release state is unknown; refusing mutation')
            return None
        return json.loads(result.stdout)

    artifact = gate()  # No release mutation can precede this proof.
    with tempfile.TemporaryDirectory(prefix='shin-qualified-release-') as folder:
        directory = Path(folder) / 'assets'
        command('gh', 'run', 'download', str(run_id), '--repo', repo, '--name', artifact, '--dir', str(directory))
        expected = verify_assets(directory, version, (root / 'VERSION').read_text().strip())
        notes += f'\nQualified source: `{sha}`. [CI qualification](https://github.com/{repo}/actions/runs/{run_id}).\n'
        notes += f"\nArchive SHA-256: `{expected[f'shin-local-{version}-macos-arm64.zip'][7:]}`.\n"
        body = Path(folder) / 'notes.md'
        body.write_text(notes)
        current = receipt()
        if current is not None:
            release_matches(current, sha, expected, complete=not current['isDraft'])
            if not current['isDraft']:
                require(current['isPrerelease'], 'Existing release is not a preview')
                return current['url']
        else:
            try:
                command('gh', 'release', 'create', tag, '--repo', repo, '--target', sha, '--draft', '--prerelease',
                        '--title', f'Shin {version} preview', '--notes-file', str(body), '--verify-tag')
            except subprocess.SubprocessError:
                pass  # Read back an uncertain creation; never blindly create again.
            current = receipt()
            require(current is not None and current['isDraft'], 'Draft creation did not confirm completion')
            release_matches(current, sha, expected, complete=False)
        uploaded = {a['name'] for a in current['assets']}
        for name in sorted(expected.keys() - uploaded):
            try:
                command('gh', 'release', 'upload', tag, '--repo', repo, str(directory / name))
            except subprocess.SubprocessError:
                pass
            current = receipt()
            require(current is not None, 'Release disappeared during upload')
            release_matches(current, sha, expected, complete=False)
            require(name in {a['name'] for a in current['assets']}, 'Upload did not confirm completion')
        gate()  # Recheck attempt, tag and matrix immediately before visibility.
        current = receipt()
        release_matches(current, sha, expected)
        try:
            command('gh', 'release', 'edit', tag, '--repo', repo, '--notes-file', str(body), '--draft=false', '--prerelease')
        except subprocess.SubprocessError:
            pass
        current = receipt()
        require(current is not None and not current['isDraft'] and current['isPrerelease'], 'Publication did not confirm completion')
        release_matches(current, sha, expected)
        return current['url']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'sha', 'tag'):
        parser.add_argument('--' + name, required=True)
    for name in ('run-id', 'attempt'):
        parser.add_argument('--' + name, type=int, required=True)
    parser.add_argument('--source-root', type=Path, help='Checkout of the exact qualified source commit')
    parser.add_argument('--recover-publisher', action='store_true',
                        help='Recover a completed run whose only failing job was publication')
    args = parser.parse_args()
    print(publish(args.repo, args.sha, args.tag, args.run_id, args.attempt,
                  args.source_root, args.recover_publisher))
