#!/usr/bin/env python3
"""Move bounded compiler data between qualified tag jobs, never qualification proof.

GitHub's ordinary cache is tag-scoped. This optional artifact transport reuses
the existing CI/artifact service without a second build or a privileged workflow.
Any unavailable or rejected cache is a cold build, not a release-gate bypass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request

SCHEMA = 'shin.compiler-cache.v1'
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_CONTENT = 1024 * 1024 * 1024
MAX_FILES = 20000
TAG = re.compile(r'v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-(?:dev|rc)\.(?:0|[1-9][0-9]*))?')
SHA = re.compile(r'[0-9a-f]{40}')
JOBS = {
    ('linux', 'core'): 'native (ubuntu-24.04, core)',
    ('linux', 'observer'): 'native (ubuntu-24.04, observer)',
    ('macos', 'core'): 'native (macos-15, core)',
    ('macos', 'observer'): 'native (macos-15, observer)',
    ('windows', 'native'): 'windows (native)',
    ('windows', 'desktop'): 'windows (desktop)',
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def identity(engine, compiler, component, env=os.environ):
    platform = env['RUNNER_OS'].lower()
    arch = env['RUNNER_ARCH'].lower()
    require((platform, component) in JOBS and arch in ('x64', 'arm64'), 'unsupported-job')
    require(engine == ('sccache' if platform == 'windows' else 'ccache'), 'wrong-cache-engine')
    tool = subprocess.check_output([engine, '--version'], text=True, timeout=15).splitlines()[0]
    require(0 < len(tool) < 160 and tool.isprintable(), 'invalid-engine-version')
    value = dict(platform=platform, arch=arch, component=component, engine=engine,
                 engine_version=tool, compiler_sha256=sha256(compiler), configuration='Release')
    if platform == 'windows':
        # cl.exe dispatches to separate frontends and code generation DLLs.
        value['msvc_support_sha256'] = {name: sha256(Path(compiler).parent / name)
                                       for name in ('c1.dll', 'c1xx.dll', 'c2.dll')}
    key = hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()[:24]
    name = f'shin-compiler-cache-v1-{platform}-{arch}-{component}-{key}'
    return value, name


def origin(env, ident):
    repo = env.get('GITHUB_REPOSITORY', '')
    tag = env.get('GITHUB_REF_NAME', '')
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo), 'invalid-repository')
    require(env.get('GITHUB_EVENT_NAME') in ('push', 'workflow_dispatch') and
            env.get('GITHUB_REF_TYPE') == 'tag' and TAG.fullmatch(tag), 'not-a-trusted-tag')
    require(SHA.fullmatch(env.get('GITHUB_SHA', '')), 'invalid-source-sha')
    run_id, attempt = int(env['GITHUB_RUN_ID']), int(env['GITHUB_RUN_ATTEMPT'])
    require(run_id > 0 and attempt > 0, 'invalid-run-identity')
    return dict(repository=repo, sha=env['GITHUB_SHA'], tag=tag, run_id=run_id,
                attempt=attempt, job=JOBS[(ident['platform'], ident['component'])])


class Api:
    def __init__(self, token):
        class Redirects(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):
                parsed = urllib.parse.urlsplit(newurl)
                require(parsed.scheme == 'https' and parsed.netloc == 'api.github.com', 'unsafe-api-redirect')
                return super().redirect_request(req, fp, code, msg, headers, newurl)
        self.opener = urllib.request.build_opener(Redirects())
        self.token = token
        self.deadline = time.monotonic() + 20

    def __call__(self, route):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, 'cache-lookup-budget-exceeded')
        request = urllib.request.Request('https://api.github.com/' + route, headers={
            'Authorization': 'Bearer ' + self.token, 'Accept': 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2026-03-10', 'User-Agent': 'Shin-compiler-cache'})
        with self.opener.open(request, timeout=min(5, remaining)) as response:
            data = response.read(2 * 1024 * 1024 + 1)
        require(len(data) <= 2 * 1024 * 1024, 'api-response-too-large')
        return json.loads(data)


def tag_sha(fetch, repo, tag):
    obj = fetch(f'repos/{repo}/git/ref/tags/{tag}')['object']
    for _ in range(4):
        require(SHA.fullmatch(obj.get('sha', '')), 'invalid-tag-object')
        if obj['type'] == 'commit':
            return obj['sha']
        require(obj['type'] == 'tag', 'unsupported-tag-object')
        obj = fetch(f'repos/{repo}/git/tags/{obj["sha"]}')['object']
    raise ValueError('tag-depth-exceeded')


def select(fetch, repo, name, ident, current_run):
    listing = fetch(f'repos/{repo}/actions/artifacts?name={name}&per_page=20')
    expected_job = JOBS[(ident['platform'], ident['component'])]
    for artifact in sorted(listing['artifacts'], key=lambda row: row['id'], reverse=True)[:5]:
        try:
            require(artifact['name'] == name and artifact['expired'] is False and
                    0 < artifact['size_in_bytes'] <= MAX_ARCHIVE + 65536, 'invalid-artifact')
            reference = artifact['workflow_run']
            run_id = reference['id']
            require(isinstance(run_id, int) and run_id > 0 and run_id != current_run, 'wrong-run')
            run = fetch(f'repos/{repo}/actions/runs/{run_id}')
            require(run['id'] == run_id and run['repository']['full_name'] == repo and
                    run['head_repository']['full_name'] == repo and
                    run['path'] == '.github/workflows/ci.yml' and run['event'] in ('push', 'workflow_dispatch') and
                    run['conclusion'] in (None, 'success', 'failure', 'timed_out'), 'wrong-ci-provenance')
            require(TAG.fullmatch(run['head_branch']) and SHA.fullmatch(run['head_sha']) and
                    reference['head_sha'] == run['head_sha'] and
                    tag_sha(fetch, repo, run['head_branch']) == run['head_sha'], 'tag-source-mismatch')
            attempt = run['run_attempt']
            require(isinstance(attempt, int) and attempt > 0, 'invalid-attempt')
            jobs = fetch(f'repos/{repo}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100')['jobs']
            matches = [job for job in jobs if job['name'] == expected_job]
            require(len(matches) == 1 and matches[0]['status'] == 'completed' and
                    matches[0]['conclusion'] == 'success', 'unqualified-job')
            steps = matches[0]['steps']
            required = ['Build', 'Tests', 'Upload compiler cache']
            if ident['platform'] in ('linux', 'macos') and ident['component'] == 'core':
                required += ['Strict source profiles']
            if ident['component'] in ('observer', 'desktop'):
                title = {'linux': 'Linux', 'macos': 'macOS', 'windows': 'Windows'}[ident['platform']]
                required += [f'Exact {title} kit qualification', f'Upload qualified {title} kit']
            require(all(sum(step['name'] == label and step['conclusion'] == 'success' for step in steps) == 1
                        for label in required), 'missing-qualification-step')
            return dict(artifact_id=artifact['id'], artifact_name=name,
                        source=dict(repository=repo, sha=run['head_sha'], tag=run['head_branch'],
                                    run_id=run_id, attempt=attempt, job=expected_job))
        except (KeyError, TypeError, ValueError):
            continue
    return None


def safe_name(name):
    path = PurePosixPath(name)
    require(name and path.parts and len(name) <= 512 and not path.is_absolute() and
            path.as_posix() == name and all(part not in ('', '.', '..') for part in path.parts) and
            not any(ord(c) < 32 or c in '\\:' for c in name), 'unsafe-cache-path')
    return path


def export_cache(cache, output, state):
    require(state['origin'] is not None, 'not-a-trusted-tag')
    cache = Path(cache)
    require(cache.is_dir() and not cache.is_symlink(), 'cache-unavailable')
    files, total = [], 0
    for path in sorted(cache.rglob('*')):
        info = path.lstat()
        require(stat.S_ISDIR(info.st_mode) or (stat.S_ISREG(info.st_mode) and info.st_nlink == 1), 'unsafe-cache-file')
        if not stat.S_ISREG(info.st_mode):
            continue
        safe_name(path.relative_to(cache).as_posix())
        total += info.st_size
        require(len(files) < MAX_FILES and total <= MAX_CONTENT, 'cache-budget-exceeded')
        files.append(path)
    require(files, 'empty-cache')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    archive = output / 'compiler-cache.tar.gz'
    try:
        with tarfile.open(archive, 'w:gz', compresslevel=1, dereference=True) as tar:
            for path in files:
                tar.add(path, arcname=path.relative_to(cache).as_posix(), recursive=False)
        require(0 < archive.stat().st_size <= MAX_ARCHIVE, 'archive-budget-exceeded')
        value = dict(schema_version=SCHEMA, identity=state['identity'], source=state['origin'],
                     archive=dict(sha256=sha256(archive), files=len(files), content_bytes=total,
                                  bytes=archive.stat().st_size))
        (output / 'compiler-cache.json').write_text(json.dumps(value, sort_keys=True) + '\n')
    except Exception:
        shutil.rmtree(output)
        raise


def restore_cache(bundle, target, state):
    require(state['selected'] is not None, 'no-qualified-cache')
    bundle, target = Path(bundle), Path(target)
    metadata = bundle / 'compiler-cache.json'
    archive = bundle / 'compiler-cache.tar.gz'
    require(not metadata.is_symlink() and metadata.stat().st_size <= 16384 and
            not archive.is_symlink() and 0 < archive.stat().st_size <= MAX_ARCHIVE, 'invalid-cache-bundle')
    value = json.loads(metadata.read_text())
    require(set(value) == {'schema_version', 'identity', 'source', 'archive'} and value['schema_version'] == SCHEMA and
            value['identity'] == state['identity'] and value['source'] == state['selected']['source'], 'cache-binding-mismatch')
    expected = value['archive']
    require(expected['bytes'] == archive.stat().st_size and expected['sha256'] == sha256(archive), 'cache-checksum-mismatch')
    require(not target.is_symlink() and (not target.exists() or (target.is_dir() and not any(target.iterdir()))),
            'cache-destination-not-empty')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shin-cache-', dir=target.parent) as folder:
        stage = Path(folder) / 'cache'
        stage.mkdir()
        files, total, names = 0, 0, set()
        with tarfile.open(archive, 'r:gz') as tar:
            for member in tar:
                path = safe_name(member.name)
                require(member.isfile() and member.name not in names and 0 <= member.size <= MAX_CONTENT,
                        'unsafe-cache-member')
                names.add(member.name)
                files += 1
                total += member.size
                require(files <= MAX_FILES and total <= MAX_CONTENT, 'cache-budget-exceeded')
                destination = stage.joinpath(*path.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as source, destination.open('xb') as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                destination.chmod(0o600)
        require(files == expected['files'] and total == expected['content_bytes'], 'cache-inventory-mismatch')
        if target.exists():
            target.rmdir()
        stage.rename(target)
    return dict(files=files, content_bytes=total)


def outputs(**values):
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            for key, value in values.items():
                require('\n' not in str(value) and '\r' not in str(value), 'invalid-action-output')
                stream.write(f'{key}={value}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'export', 'restore'))
    parser.add_argument('--state', type=Path, required=True)
    parser.add_argument('--engine', choices=('ccache', 'sccache'))
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--component')
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--bundle', type=Path)
    args = parser.parse_args()
    status = dict(operation=args.operation, status='miss')
    try:
        if args.operation == 'prepare':
            ident, name = identity(args.engine, args.compiler, args.component)
            try:
                own = origin(os.environ, ident)
            except (KeyError, ValueError):
                own = None
            state = dict(identity=ident, artifact_name=name, origin=own, selected=None)
            try:
                token = os.environ.get('GH_TOKEN', '')
                if token:
                    state['selected'] = select(Api(token), os.environ['GITHUB_REPOSITORY'], name,
                                               ident, int(os.environ['GITHUB_RUN_ID']))
            except (OSError, KeyError, TypeError, ValueError):
                status['reason'] = 'cache-api-unavailable'
            args.state.write_text(json.dumps(state, sort_keys=True) + '\n')
            source = state['selected']
            outputs(artifact_name=name, artifact_id=source['artifact_id'] if source else '',
                    source_run=source['source']['run_id'] if source else '')
            status['status'] = 'selected' if source else 'miss'
        elif args.operation == 'export':
            state = json.loads(args.state.read_text())
            export_cache(args.cache, args.bundle, state)
            outputs(ready='true')
            status['status'] = 'exported'
        else:
            state = json.loads(args.state.read_text())
            status.update(restore_cache(args.bundle, args.cache, state), status='restored')
            outputs(restored='true')
    except (OSError, EOFError, KeyError, TypeError, ValueError, tarfile.TarError, subprocess.SubprocessError):
        # Compiler caches are optional. Never convert a cache failure into a
        # successful source/package check or reuse an old qualification receipt.
        status['reason'] = 'cache-unavailable-or-rejected'
    print(json.dumps(status, sort_keys=True))


if __name__ == '__main__':
    main()
