#!/usr/bin/env python3
"""Publish each exact platform kit after its own source and package qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import zipfile

PLATFORMS = {
    'linux': dict(jobs={'native (ubuntu-24.04, core)', 'native (ubuntu-24.04, observer)'},
                  package_job='native (ubuntu-24.04, observer)',
                  steps=('Exact Linux kit qualification', 'Upload qualified Linux kit'),
                  artifact='shin-terminal-linux-X64'),
    'macos': dict(jobs={'native (macos-15, core)', 'native (macos-15, observer)'},
                  package_job='native (macos-15, observer)',
                  steps=('Exact macOS kit qualification', 'Upload qualified macOS kit'),
                  artifact='shin-local-macos-ARM64'),
    'windows': dict(jobs={'windows (native)', 'windows (desktop)'},
                    package_job='windows (desktop)',
                    steps=('Exact Windows kit qualification', 'Upload qualified Windows kit'),
                    artifact='shin-desktop-windows-X64'),
}
MATRIX = set().union(*(spec['jobs'] for spec in PLATFORMS.values()))
VERSION = r'(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})-dev\.(?:0|[1-9][0-9]{0,8})'


def require(condition, message):
    if not condition:
        raise ValueError(message)


class PackageProofPending(ValueError):
    """Completed-job step metadata has not reached the API yet."""


def qualification(run, jobs, artifacts, repo, sha, run_id, attempt, recover_publisher=False,
                  platform='macos', existing=False):
    require(platform in PLATFORMS, 'Unknown platform')
    require(run['head_sha'] == sha and run['id'] == run_id and run['run_attempt'] == attempt,
            'CI identity or attempt differs from the requested source')
    require(run['repository']['full_name'] == repo and run['head_repository']['full_name'] == repo
            and run['event'] == 'push' and run['path'] == '.github/workflows/ci.yml', 'Wrong CI provenance')
    # A queued or failed unrelated platform cannot invalidate a qualified kit.
    # An explicit cancellation still prevents a new publication. Previously
    # published bytes remain verifiable through their exact historical attempt.
    if not existing:
        require(run['status'] in ('queued', 'in_progress', 'completed', 'waiting', 'pending')
                and run['conclusion'] in (None, 'success', 'failure', 'timed_out'),
                'CI run is not eligible for publication')
        publishers = [j for j in jobs if j['name'] == f'publish-preview ({platform})']
        require(len(publishers) == 1, 'Platform publisher is missing or ambiguous')
        publisher = publishers[0]
        require(publisher['status'] == 'in_progress' or
                (publisher['status'] == 'completed' and publisher['conclusion'] == 'success') or
                (recover_publisher and publisher['status'] == 'completed'
                 and publisher['conclusion'] == 'failure'), 'Platform publisher is not eligible')
    spec = PLATFORMS[platform]
    for name in spec['jobs']:
        matches = [job for job in jobs if job['name'] == name]
        require(len(matches) == 1 and matches[0]['status'] == 'completed'
                and matches[0]['conclusion'] == 'success', f'Unqualified matrix job: {name}')
    job = next(job for job in jobs if job['name'] == spec['package_job'])
    for step in spec['steps']:
        matches = [row for row in job['steps'] if row['name'] == step]
        if not matches or (len(matches) == 1 and matches[0]['conclusion'] is None):
            raise PackageProofPending(f'Package proof metadata is pending: {step}')
        require(len(matches) == 1 and matches[0]['conclusion'] == 'success', f'Missing package proof: {step}')
    require(len(artifacts) == 1 and {a['name'] for a in artifacts} == artifact_names(run_id, attempt, platform),
            'Qualified platform artifact is missing or ambiguous')
    for artifact in artifacts:
        require(not artifact['expired'] and artifact['workflow_run']['id'] == run_id
                and artifact['workflow_run']['head_sha'] == sha,
                'Artifact does not belong to this qualified run')


def artifact_names(run_id, attempt, platform=None):
    return {f"{spec['artifact']}-{run_id}-{attempt}" for name, spec in PLATFORMS.items()
            if platform is None or name == platform}


def verify_windows_assets(directory, version, core, sha, run_id, attempt):
    stem = f'shin-desktop-{version}-windows-AMD64'
    names = {stem + suffix for suffix in ('.zip', '.zip.sha256', '.msi', '.msi.sha256')}
    require({p.name for p in directory.iterdir()} == names | {'windows-qualification.json'},
            'Expected qualified Windows ZIP, MSI, checksums and receipt')
    expected = {}
    for name in names:
        path = directory / name
        require(0 < path.stat().st_size <= 128 * 1024 * 1024, 'Windows asset exceeds release bound')
        expected[name] = 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()
    for suffix in ('.zip', '.msi'):
        name = stem + suffix
        require((directory / (name + '.sha256')).read_text() in
                (f'{expected[name][7:]}  {name}', f'{expected[name][7:]}  {name}\n'), 'Windows checksum mismatch')
    require((directory / (stem + '.msi')).read_bytes()[:8] == bytes.fromhex('d0cf11e0a1b11ae1'),
            'Invalid Windows Installer database')
    receipt = json.loads((directory / 'windows-qualification.json').read_text())
    require(receipt == dict(schema_version='shin.windows-qualification.v1', version=version, core_version=core,
                           render_scope='headless',
                           source_sha=sha, run_id=run_id, attempt=attempt, assets=expected,
                           checks=['msi-metadata', 'msi-payload', 'archive-inventory', 'unelevated-runtime', 'installed-update-preflight']),
            'Windows qualification receipt differs from this source, attempt or package')
    with zipfile.ZipFile(directory / (stem + '.zip')) as kit:
        prefix = stem + '/'
        entries = kit.infolist()
        require(len(entries) <= 2048 and len({i.filename for i in entries}) == len(entries),
                'Duplicate or excessive Windows archive members')
        require(all(i.filename.startswith(prefix) and '..' not in Path(i.filename).parts
                    and '\\' not in i.filename and ':' not in i.filename
                    and (i.external_attr >> 16) & 0o170000 != 0o120000 for i in entries), 'Unsafe Windows kit paths')
        require(sum(i.file_size for i in entries) <= 512 * 1024 * 1024, 'Windows payload exceeds release bound')
        manifest = json.loads(kit.read(prefix + 'shin-kit.json'))
        require(manifest['schema_version'] == 'shin.local-kit.v1' and manifest['version'] == version
                and manifest['core_version'] == core, 'Windows kit component identity mismatch')
        paths = [row['path'] for row in manifest['files']]
        require(0 < len(paths) <= 1024 and len(set(paths)) == len(paths), 'Invalid Windows inventory')
        require({prefix + p for p in paths} | {prefix + 'shin-kit.json'} ==
                {i.filename for i in entries if not i.is_dir()}, 'Unlisted Windows payload')
        require({'bin/shin-ce.exe', 'bin/shin-context.exe', 'bin/shin-observer.exe', 'bin/shin-mcp.exe',
                 'bin/shin-tray.exe', 'bin/vcruntime140.dll', 'bin/msvcp140.dll'} <= set(paths),
                'Incomplete Windows runtime')
        for row in manifest['files']:
            require(not Path(row['path']).is_absolute(), 'Unsafe Windows manifest path')
            value = kit.read(prefix + row['path'])
            require(len(value) == row['size'] and hashlib.sha256(value).hexdigest() == row['sha256'],
                    'Windows manifest hash mismatch')
    expected['windows-qualification.json'] = 'sha256:' + hashlib.sha256((directory / 'windows-qualification.json').read_bytes()).hexdigest()
    return expected


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
    require(receipt['source_sha'] == sha, 'Existing release targets different source')
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


def mutation(*args):
    """Attempt once; callers must confirm the outcome with a separate read."""
    try:
        command(*args)
    except subprocess.SubprocessError as error:
        detail = getattr(error, 'stderr', None) or str(error)
        if isinstance(detail, bytes):
            detail = detail.decode('utf-8', errors='replace')
        # Preserve CLI/API failures without terminal escapes or unbounded logs.
        return ''.join(c for c in detail[-4096:] if c.isprintable() or c == '\n').strip()
    return ''


def platform_assets(platform, version):
    if platform == 'macos':
        name = f'shin-local-{version}-macos-arm64.zip'
        return {name, name + '.sha256', 'install.sh',
                'shin-updater-v1-macos-arm64.zip', 'shin-updater-v1-macos-arm64.zip.sha256'}
    if platform == 'windows':
        stem = f'shin-desktop-{version}-windows-AMD64'
        return {stem + suffix for suffix in ('.zip', '.zip.sha256', '.msi', '.msi.sha256')} | {'windows-qualification.json'}
    name = f'shin-terminal-{version}-linux-x86_64.tar.gz'
    return {name, name + '.sha256', 'linux-qualification.json'}


def verify_linux_assets(directory, version, core, sha, run_id, attempt):
    from linux_package import CHECKS, qualification_receipt, verify_archive
    require({p.name for p in directory.iterdir()} == platform_assets('linux', version),
            'Expected qualified Linux archive, checksum and receipt')
    archive = directory / f'shin-terminal-{version}-linux-x86_64.tar.gz'
    manifest = verify_archive(archive, version, core)
    expected = {p.name: 'sha256:' + hashlib.sha256(p.read_bytes()).hexdigest()
                for p in directory.iterdir() if p.name != 'linux-qualification.json'}
    receipt = json.loads((directory / 'linux-qualification.json').read_text())
    require(receipt == qualification_receipt(archive, manifest, sha, run_id, attempt, receipt['runtime'], CHECKS),
            'Linux qualification receipt differs from this source, attempt or package')
    expected['linux-qualification.json'] = 'sha256:' + hashlib.sha256((directory / 'linux-qualification.json').read_bytes()).hexdigest()
    return expected


def marker_name(platform):
    return f'shin-preview-{platform}.json'


def marker(repo, sha, version, core, platform, run_id, attempt, assets):
    return dict(schema_version='shin.preview-platform.v1', repository=repo, source_sha=sha,
                version=version, core_version=core, platform=platform, run_id=run_id, attempt=attempt,
                assets=assets)


def marker_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def upload_order(names, platform, version):
    # A release can already be public for another platform. Upload the macOS
    # kit ZIP last: existing clients must not see its ZIP+checksum pair before
    # the independent updater and installer are present. The marker follows it.
    order = sorted(names - {marker_name(platform)})
    if platform == 'macos':
        archive = f'shin-local-{version}-macos-arm64.zip'
        order.remove(archive)
        order.append(archive)
    return order + [marker_name(platform)]


def publish(repo, sha, tag, run_id, attempt, source_root=None, recover_publisher=False, platform='macos'):
    require(platform in PLATFORMS, 'Unknown platform')
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo), 'Invalid repository')
    require(re.fullmatch(r'[0-9a-f]{40}', sha) and re.fullmatch('v' + VERSION, tag), 'Invalid source or preview tag')
    require(run_id > 0 and attempt > 0, 'Invalid CI operation')
    root = Path(source_root).resolve() if source_root else Path(__file__).resolve().parents[1]
    version = tag[1:]
    core = (root / 'VERSION').read_text().strip()
    require((root / 'observer/VERSION').read_text().strip() == version, 'Tag differs from Observer VERSION')
    require(command('git', '-C', str(root), 'rev-parse', 'HEAD').strip() == sha, 'Checkout differs from CI source')
    notes = (root / 'docs/releases' / (tag + '.md')).read_text()

    def api(path):
        return json.loads(command('gh', 'api', f'repos/{repo}/{path}'))

    def gate_once(target, target_run, target_attempt, existing=False):
        run = api(f'actions/runs/{target_run}/attempts/{target_attempt}')
        page = api(f'actions/runs/{target_run}/attempts/{target_attempt}/jobs?per_page=100')
        require(page['total_count'] == len(page['jobs']), 'CI job list is incomplete')
        artifacts = api(f'actions/runs/{target_run}/artifacts?per_page=100')
        require(artifacts['total_count'] == len(artifacts['artifacts']), 'Artifact list is incomplete')
        selected = [a for a in artifacts['artifacts'] if a['name'] in artifact_names(target_run, target_attempt, target)]
        require(run['head_branch'] == tag, 'Qualification did not run for this preview tag')
        qualification(run, page['jobs'], selected, repo, sha, target_run, target_attempt,
                      recover_publisher, target, existing)
        require(api(f'commits/{tag}')['sha'] == sha, 'Release tag moved away from qualified source')
        return selected[0]['name']

    def gate(target, target_run, target_attempt, existing=False):
        for retry in range(10):
            try:
                return gate_once(target, target_run, target_attempt, existing)
            except PackageProofPending:
                if retry == 9:
                    raise
                time.sleep(3)  # Read-only metadata convergence, never mutation replay.

    def receipt():
        result = subprocess.run(['gh', 'release', 'view', tag, '--repo', repo, '--json',
                                 'isDraft,isPrerelease,tagName,assets,url,body'], capture_output=True, text=True, timeout=30)
        if result.returncode:
            require('release not found' in result.stderr.lower(), 'Release state is unknown; refusing mutation')
            return None
        value = json.loads(result.stdout)
        require(value['tagName'] == tag, 'Release tag differs from qualified preview')
        # target_commitish is only a tag-creation hint; GitHub ignores it for
        # an existing tag. Resolve the actual release tag on every readback.
        value['source_sha'] = api(f'commits/{tag}')['sha']
        require(value['source_sha'] == sha, 'Release tag moved away from qualified source')
        return value

    gate(platform, run_id, attempt)  # No download or release mutation precedes this proof.
    with tempfile.TemporaryDirectory(prefix='shin-qualified-release-') as folder:
        directory = Path(folder)
        checked = {}
        def qualified(target, target_run, target_attempt, existing=False):
            key = (target, target_run, target_attempt)
            if key not in checked:
                artifact = gate(target, target_run, target_attempt, existing)
                destination = directory / f'{target}-{target_run}-{target_attempt}'
                command('gh', 'run', 'download', str(target_run), '--repo', repo,
                        '--name', artifact, '--dir', str(destination))
                if target == 'macos':
                    assets = verify_assets(destination, version, core)
                elif target == 'windows':
                    assets = verify_windows_assets(destination, version, core, sha, target_run, target_attempt)
                else:
                    assets = verify_linux_assets(destination, version, core, sha, target_run, target_attempt)
                proof = marker(repo, sha, version, core, target, target_run, target_attempt, assets)
                value = marker_bytes(proof)
                (destination / marker_name(target)).write_bytes(value)
                checked[key] = (destination, {**assets, marker_name(target): 'sha256:' + hashlib.sha256(value).hexdigest()}, proof)
            return checked[key]

        def existing_assets(current):
            allowed = {}
            completed = set()
            if current is None:
                return allowed, completed
            require(current['isPrerelease'], 'Existing release is not a preview')
            require(current['source_sha'] == sha, 'Existing release targets different source')
            names = {a['name'] for a in current['assets']}
            require(len(names) == len(current['assets']), 'Duplicate release assets')
            for target in PLATFORMS:
                name = marker_name(target)
                if name in names:
                    location = Path(tempfile.mkdtemp(prefix='existing-' + target + '-', dir=directory))
                    command('gh', 'release', 'download', tag, '--repo', repo, '--pattern', name, '--dir', str(location))
                    raw = (location / name).read_bytes()
                    require(len(raw) <= 32768, 'Platform receipt exceeds bound')
                    proof = json.loads(raw)
                    require(type(proof.get('run_id')) is int and type(proof.get('attempt')) is int
                            and proof['run_id'] > 0 and proof['attempt'] > 0, 'Invalid platform provenance')
                    _, expected, identity = qualified(target, proof['run_id'], proof['attempt'], True)
                    require(proof == identity and raw == marker_bytes(identity), 'Existing platform receipt differs from qualified source')
                    allowed.update(expected)
                    require(platform_assets(target, version) | {name} <= names, 'Published platform assets are incomplete')
                    completed.add(target)
                elif names & platform_assets(target, version):
                    # Resume an interrupted same-attempt upload only when its
                    # exact files are still backed by qualified CI artifacts.
                    _, expected, _ = qualified(target, run_id, attempt, target != platform)
                    allowed.update(expected)
            release_matches(current, sha, allowed, complete=False)
            return allowed, completed

        current = receipt()
        allowed, completed = existing_assets(current)
        if platform in completed:
            previous = [entry for entry in checked.values() if entry[2]['platform'] == platform]
            require(len(previous) == 1, 'Published platform provenance is ambiguous')
            destination, own, _ = previous[0]
        else:
            destination, own, _ = qualified(platform, run_id, attempt)
        allowed.update(own)
        if current is None:
            body = directory / 'notes.md'
            body.write_text(notes)
            gate(platform, run_id, attempt)
            # The verified tag already exists. Sending --target can make
            # GITHUB_TOKEN fail GitHub's ref/workflow permission checks.
            failure = mutation('gh', 'release', 'create', tag, '--repo', repo, '--draft', '--prerelease',
                               '--title', f'Shin {version} preview', '--notes-file', str(body), '--verify-tag')
            current = receipt()
            require(current is not None and current['isDraft'] and current['isPrerelease'],
                    f'Draft creation did not confirm completion: {failure or "CLI returned success"}')
        release_matches(current, sha, allowed, complete=False)
        uploaded = {a['name'] for a in current['assets']}
        for name in upload_order(set(own), platform, version):
            if name in uploaded:
                continue
            gate(platform, run_id, attempt)
            failure = mutation('gh', 'release', 'upload', tag, '--repo', repo, str(destination / name))
            current = receipt()
            require(current is not None and current['isPrerelease'], 'Release disappeared during upload')
            release_matches(current, sha, allowed, complete=False)
            uploaded = {a['name'] for a in current['assets']}
            require(name in uploaded, f'Upload did not confirm completion: {failure or "CLI returned success"}')
        gate(platform, run_id, attempt)
        current = receipt()
        release_matches(current, sha, allowed, complete=False)
        require(set(own) <= {a['name'] for a in current['assets']}, 'Platform assets are incomplete')
        _, completed = existing_assets(current)
        notes += f'\nQualified source: `{sha}`. Platform assets appear independently after their own CI source and exact-archive checks pass.\n'
        notes += '\n| Platform | Availability |\n| --- | --- |\n'
        for target in PLATFORMS:
            notes += f"| {target} | {'Qualified downloads available' if target in completed else 'Not published for this version'} |\n"
        notes += '\nEach `shin-preview-PLATFORM.json` binds its downloads to the exact source, CI run, attempt and SHA-256 values.\n'
        body = directory / 'notes.md'
        body.write_text(notes)
        if not current['isDraft'] and current.get('body') == notes:
            return current['url']
        gate(platform, run_id, attempt)
        failure = mutation('gh', 'release', 'edit', tag, '--repo', repo, '--notes-file', str(body), '--draft=false', '--prerelease')
        current = receipt()
        require(current is not None and not current['isDraft'] and current['isPrerelease'],
                f'Publication did not confirm completion: {failure or "CLI returned success"}')
        require(current.get('body') == notes, f'Release notes update did not confirm completion: {failure or "CLI returned success"}')
        release_matches(current, sha, allowed, complete=False)
        require(set(own) <= {a['name'] for a in current['assets']}, 'Published platform assets are incomplete')
        return current['url']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'sha', 'tag'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--platform', choices=PLATFORMS, required=True)
    for name in ('run-id', 'attempt'):
        parser.add_argument('--' + name, type=int, required=True)
    parser.add_argument('--source-root', type=Path, help='Checkout of the exact qualified source commit')
    parser.add_argument('--recover-publisher', action='store_true', help='Resume this platform publisher after a failed publication')
    args = parser.parse_args()
    print(publish(args.repo, args.sha, args.tag, args.run_id, args.attempt,
                  args.source_root, args.recover_publisher, args.platform))
