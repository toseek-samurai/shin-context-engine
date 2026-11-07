#!/usr/bin/env python3
"""Build and qualify the Ubuntu 24.04 x86_64 terminal archive in disposable state."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ['archive_structure', 'manifest_inventory', 'runtime_elf', 'system_dependencies',
          'binary_versions', 'core_workflow', 'mcp_snapshot_binding', 'terminal_en_ru']
PROGRAMS = ('shin-context', 'shin-observer', 'shin-ce', 'shin-mcp', 'shin-usage')
MANIFEST = 'shin-terminal-kit.json'
RUNTIME = dict(distribution='ubuntu', minimum_distribution_version='24.04', glibc_minimum='2.39',
               system_packages=['libc6', 'libstdc++6', 'libssl3t64', 'libsqlite3-0'])
MAX_ARCHIVE = 128 * 1024 * 1024
MAX_FILE = 128 * 1024 * 1024
MAX_PAYLOAD = 512 * 1024 * 1024
MAX_FILES = 1024
MAX_MEMBERS = 2048
VERSION = r'(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})(?:-(?:dev|rc)\.(?:0|[1-9][0-9]{0,8}))?'
REQUIRED_FILES = {'bin/' + name for name in PROGRAMS}
for component in ('shin-context', 'shin-observer'):
    REQUIRED_FILES.update('share/' + component + '/' + name for name in ('LICENSE', 'NOTICE', 'VERSION'))
    for dependency, names in {
            'nlohmann': ('LICENSE.MIT',), 'simdjson': ('LICENSE-APACHE-2.0', 'LICENSE-MIT'),
            'tree-sitter': ('LICENSE', 'UPSTREAM.json'), 'tree-sitter-java': ('LICENSE', 'UPSTREAM.json'),
            'tree-sitter-typescript': ('LICENSE', 'UPSTREAM.json'), 'tree-sitter-cpp': ('LICENSE', 'UPSTREAM.json'),
            'pugixml': ('LICENSE.MIT', 'UPSTREAM.json'), 'utf8proc': ('LICENSE.md', 'UPSTREAM.json')}.items():
        REQUIRED_FILES.update('share/' + component + '/licenses/' + dependency + '/' + name for name in names)
    REQUIRED_FILES.update('share/' + component + '/licenses/' + dependency + '/LICENSE'
                          for dependency in ('re2', 'abseil'))
REQUIRED_FILES.update({
    'share/shin-context/licenses/tree-sitter-objcpp/LICENSE',
    'share/shin-context/licenses/tree-sitter-objcpp/UPSTREAM.json',
    'share/shin-context/licenses/tree-sitter/unicode/LICENSE',
    'share/shin-observer/licenses/tree-sitter-unicode/LICENSE',
    'share/shin-observer/licenses/cpp-httplib/LICENSE',
    'share/shin-observer/licenses/mcp-cpp-sdk/LICENSE',
    'share/shin-context/profiles/generic-source.json',
    'share/shin-context/contracts/local-session-recovery-v1.json',
    'share/shin-context/contracts/mcp-activity-v1.json'})


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stem(version):
    require(isinstance(version, str) and re.fullmatch(VERSION, version), 'Invalid component version')
    return f'shin-terminal-{version}-linux-x86_64'


def safe_path(value):
    require(isinstance(value, str) and 0 < len(value) <= 512 and
            all(ord(c) >= 32 and ord(c) != 127 for c in value) and
            '\\' not in value and ':' not in value, 'Unsafe archive path')
    require(not PurePosixPath(value).is_absolute() and
            all(part not in ('', '.', '..') for part in value.split('/')), 'Unsafe archive path')
    return value


def asset_hashes(archive):
    checksum = archive.with_name(archive.name + '.sha256')
    require(archive.is_file() and not archive.is_symlink() and 0 < archive.stat().st_size <= MAX_ARCHIVE,
            'Archive exceeds the release bound or is not a regular file')
    require(checksum.is_file() and not checksum.is_symlink() and checksum.stat().st_size < 512,
            'Missing or oversized archive checksum')
    value = digest(archive)
    require(checksum.read_text(encoding='utf-8') in (f'{value}  {archive.name}', f'{value}  {archive.name}\n'),
            'Linux archive checksum mismatch')
    return {archive.name: 'sha256:' + value, checksum.name: 'sha256:' + digest(checksum)}


def verify_tar_stream(archive):
    """Bound decompression and extension headers, and reject data hidden after TAR EOF."""
    count, total = 0, 0
    with gzip.open(archive, 'rb') as source:
        while True:
            header = source.read(512)
            require(len(header) == 512, 'Truncated Linux TAR header')
            if header == bytes(512):
                tail = source.read(10241)
                require(512 <= len(tail) <= 10240 and not any(tail), 'Unexpected payload after Linux TAR EOF')
                return
            member = tarfile.TarInfo.frombuf(header, 'utf-8', 'strict')
            count += 1
            require(count <= MAX_MEMBERS, 'Excessive TAR headers')
            require(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE, tarfile.XHDTYPE),
                    'Links, sparse files and special TAR members are forbidden')
            require(0 <= member.size <= (16384 if member.type == tarfile.XHDTYPE else MAX_FILE),
                    'TAR member or extended metadata exceeds the release bound')
            padded = (member.size + 511) // 512 * 512
            total += 512 + padded
            require(total <= MAX_PAYLOAD + MAX_MEMBERS * 17408, 'Linux TAR stream exceeds the release bound')
            while padded:
                chunk = source.read(min(padded, 65536))
                require(chunk, 'Truncated Linux TAR payload')
                padded -= len(chunk)


def verify_archive(archive, version, core_version):
    """Validate the complete TAR inventory and checksum without executing its payload."""
    archive = Path(archive)
    prefix = stem(version)
    stem(core_version)
    require(archive.name == prefix + '.tar.gz', 'Linux archive name differs from its version')
    asset_hashes(archive)
    verify_tar_stream(archive)
    with tarfile.open(archive, 'r:gz') as kit:
        members, total = {}, 0
        for member in kit:
            name = member.name.rstrip('/') if member.isdir() else member.name
            safe_path(name)
            require(name == prefix or name.startswith(prefix + '/'), 'Wrong Linux archive root')
            require(name not in members and len(members) < MAX_MEMBERS, 'Duplicate or excessive TAR members')
            require(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE) and not member.sparse and
                    not member.mode & 0o7000, 'Links, sparse files and special TAR members are forbidden')
            require(0 <= member.size <= MAX_FILE and (not member.isdir() or member.size == 0),
                    'TAR member exceeds the release bound')
            total += member.size
            require(total <= MAX_PAYLOAD, 'Linux payload exceeds the release bound')
            members[name] = member
        manifest_name = prefix + '/' + MANIFEST
        require(manifest_name in members and members[manifest_name].isfile() and
                members[manifest_name].size <= 1024 * 1024, 'Missing or oversized terminal manifest')
        manifest = json.load(kit.extractfile(members[manifest_name]))
        require(isinstance(manifest, dict) and manifest.get('schema_version') == 'shin.terminal-kit.v1' and
                manifest.get('version') == version and manifest.get('core_version') == core_version and
                manifest.get('platform') == 'linux' and manifest.get('architecture') == 'x86_64' and
                manifest.get('runtime') == RUNTIME, 'Linux component identity or runtime declaration mismatch')
        rows = manifest.get('files')
        require(isinstance(rows, list) and 0 < len(rows) <= MAX_FILES, 'Invalid Linux inventory')
        paths = set()
        for row in rows:
            require(isinstance(row, dict) and set(row) == {'path', 'size', 'sha256', 'executable'},
                    'Invalid Linux inventory entry')
            path = safe_path(row['path'])
            require(path not in paths and path != MANIFEST, 'Duplicate Linux inventory entry')
            paths.add(path)
            require(type(row['size']) is int and 0 <= row['size'] <= MAX_FILE and
                    type(row['executable']) is bool and isinstance(row['sha256'], str) and
                    re.fullmatch('[0-9a-f]{64}', row['sha256']), 'Invalid Linux inventory metadata')
            member = members.get(prefix + '/' + path)
            require(member is not None and member.isfile() and member.size == row['size'] and
                    bool(member.mode & 0o111) == row['executable'], 'Linux inventory size or mode mismatch')
            with kit.extractfile(member) as stream:
                require(hashlib.file_digest(stream, 'sha256').hexdigest() == row['sha256'],
                        'Linux inventory hash mismatch')
        require(REQUIRED_FILES <= paths, 'Incomplete Linux runtime, profiles, contracts or license material')
        require({p for p in paths if p.startswith('bin/')} == {'bin/' + p for p in PROGRAMS},
                'Unexpected Linux runtime executable or bundled library')
        require(all(row['executable'] for row in rows if row['path'].startswith('bin/')),
                'Linux runtime programs must be executable')
        files = {prefix + '/' + p for p in paths | {MANIFEST}}
        directories = {prefix}
        for path in files:
            directories.update(str(p) for p in PurePosixPath(path).parents if str(p) != '.')
        require({n for n, m in members.items() if m.isfile()} == files and
                {n for n, m in members.items() if m.isdir()} <= directories,
                'Unlisted Linux archive payload')
        for relative, expected in (('share/shin-observer/VERSION', version), ('share/shin-context/VERSION', core_version)):
            require(kit.extractfile(members[prefix + '/' + relative]).read().decode('utf-8').strip() == expected,
                    'Installed component VERSION differs from the manifest')
    return manifest


def extract_archive(archive, destination, manifest):
    """Only materialize already validated regular files, never TAR ownership or links."""
    prefix = stem(manifest['version'])
    target = destination / prefix
    target.mkdir()
    with tarfile.open(archive, 'r:gz') as kit:
        for relative in [MANIFEST, *(row['path'] for row in manifest['files'])]:
            member = kit.getmember(prefix + '/' + relative)
            path = target / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            with kit.extractfile(member) as source, path.open('xb') as output:
                shutil.copyfileobj(source, output)
            path.chmod(0o755 if member.mode & 0o111 else 0o644)
    return target


def checked_run(arguments, *, env=None, cwd=None, timeout=45):
    child = subprocess.Popen([str(a) for a in arguments], env=env, cwd=cwd, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        try:
            output, errors = child.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Let the Python PTY gate run its finally blocks before terminating
            # this command's process group and any owned service descendants.
            child.send_signal(signal.SIGINT)
            try:
                child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                kill_group(child)
                child.communicate(timeout=5)
            raise
    finally:
        kill_group(child)
        if child.poll() is None:
            child.communicate(timeout=5)
    require(child.returncode == 0 and len(output) <= 4 * 1024 * 1024,
            f'{Path(arguments[0]).name} failed: {errors[-2000:]}')
    return output


def kill_group(child):
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def linux_runtime():
    require(sys.platform == 'linux' and platform.machine() == 'x86_64', 'Qualification requires Linux x86_64')
    release = platform.freedesktop_os_release()
    require(release.get('ID') == 'ubuntu' and release.get('VERSION_ID') == '24.04',
            'Qualification requires the Ubuntu 24.04 runtime baseline')
    libc = os.confstr('CS_GNU_LIBC_VERSION').split()
    require(len(libc) == 2 and libc[0] == 'glibc' and re.fullmatch(r'[0-9]+\.[0-9]+', libc[1]) and
            tuple(map(int, libc[1].split('.'))) >= (2, 39), 'Qualification requires glibc 2.39 or newer')
    return dict(system='linux', distribution='ubuntu', distribution_version='24.04',
                architecture='x86_64', glibc=libc[1])


def verify_elf_dependencies(prefix):
    environment = {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}
    system_dirs = {Path(p).resolve() for p in ('/lib/x86_64-linux-gnu', '/usr/lib/x86_64-linux-gnu', '/lib64', '/usr/lib64')}
    for name in PROGRAMS:
        executable = prefix / 'bin' / name
        with executable.open('rb') as stream:
            header = stream.read(64)
        require(len(header) == 64 and header[:7] == b'\x7fELF\x02\x01\x01' and
                struct.unpack_from('<HH', header, 16) in ((2, 62), (3, 62)), 'Expected a native x86_64 ELF image')
        dynamic = checked_run(['/usr/bin/readelf', '-d', executable], env=environment)
        require('(RPATH)' not in dynamic and '(RUNPATH)' not in dynamic, 'Linux runtime embeds a search prefix')
        dependencies = checked_run(['/usr/bin/ldd', executable], env=environment)
        resolved = 0
        for line in dependencies.splitlines():
            line = line.strip()
            if re.fullmatch(r'linux-vdso\.so\.1 \(0x[0-9a-f]+\)', line):
                continue
            match = re.fullmatch(r'(?:\S+ => )?(/\S+) \(0x[0-9a-f]+\)', line)
            require(match is not None, 'Unresolved or unexpected Linux runtime dependency')
            path = Path(match[1])
            require(path.parent.resolve() in system_dirs and path.resolve().parent in system_dirs and path.is_file(),
                    'Linux runtime dependency is outside system library directories')
            resolved += 1
        require(resolved > 0, 'Dynamic Linux runtime dependencies were not verified')


def mcp_workflow(prefix, root, environment, version, core_version):
    child = subprocess.Popen([str(prefix / 'bin/shin-mcp'), '--root', str(root)], cwd=root, env=environment,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    sequence, pending = 0, b''
    try:
        def rpc(method, params):
            nonlocal sequence, pending
            sequence += 1
            child.stdin.write((json.dumps(dict(jsonrpc='2.0', id=sequence, method=method, params=params)) + '\n').encode())
            child.stdin.flush()
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    response = json.loads(line)
                    if response.get('id') == sequence:
                        require('error' not in response, 'MCP JSON-RPC request failed')
                        return response['result']
                if select.select([child.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                    value = os.read(child.stdout.fileno(), 8192)
                    require(value, 'MCP closed before replying')
                    pending += value
                    require(len(pending) <= 1024 * 1024, 'MCP response exceeds the qualification bound')
            raise ValueError('MCP qualification response timed out')

        def tool(name, arguments, ok=True):
            result = rpc('tools/call', dict(name=name, arguments=arguments))
            value = json.loads(result['content'][0]['text'])
            require(value.get('ok') is ok and bool(result.get('isError', False)) is not ok,
                    'MCP tool result differs from expected success/refusal')
            return value

        hello = rpc('initialize', dict(protocolVersion='2025-11-25', capabilities={},
                    clientInfo=dict(name='shin-linux-qualification', version='1')))
        require(hello['serverInfo'] == dict(name='shin', version=version), 'MCP component version mismatch')
        child.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        child.stdin.flush()
        query = tool('shin_query', {'query': 'quotaLimit'})
        require(query['version'] == version and query['core_version'] == core_version and
                'return 17' in query['source_context'], 'MCP query did not use the extracted runtime')
        read = tool('shin_read', {'path': 'quota.cpp', 'snapshot': query['snapshot_id']})
        require(read['snapshot_id'] == query['snapshot_id'] and 'return 17' in read['source_context'],
                'MCP source read lost the query snapshot binding')
        refused = tool('shin_read', {'path': 'quota.cpp', 'snapshot': 'fnv1a64:0000000000000000'}, ok=False)
        require(refused.get('recovery', {}).get('next_action') == 'query-same-project-for-fresh-snapshot',
                'MCP accepted an unrelated source snapshot')
        child.stdin.close()
        child.stdin = None
        _, errors = child.communicate(timeout=10)
        require(child.returncode == 0 and not errors, 'MCP did not exit cleanly at EOF')
    finally:
        kill_group(child)
        if child.poll() is None:
            child.communicate(timeout=5)


def stop_qualification_service(service, endpoint, ce, root, environment):
    """Preserve an active proof failure while recording bounded shutdown evidence."""
    primary, cleanup_error, errors = sys.exc_info()[1], None, b''
    try:
        if endpoint.exists():
            stopped = json.loads(checked_run([ce, 'service-stop'], cwd=root, env=environment, timeout=15))
            require(stopped.get('ok'), 'Isolated qualification service refused to stop')
        _, errors = service.communicate(timeout=15)
        require(service.returncode == 0 and not errors and not endpoint.exists(),
                'Isolated qualification service did not stop cleanly')
    except Exception as error:
        cleanup_error = error
    finally:
        try:
            kill_group(service)
            if service.poll() is None:
                _, errors = service.communicate(timeout=5)
        except Exception as error:
            cleanup_error = cleanup_error or error
    if cleanup_error:
        detail = 'Isolated qualification service cleanup failed: ' + json.dumps(dict(
            exit_code=service.poll(), stderr_tail=errors.decode('utf-8', errors='replace')[-2000:],
            endpoint_exists=endpoint.exists(), reason=str(cleanup_error)[-2000:]))
        if primary is not None:
            primary.add_note(detail)
        else:
            raise ValueError(detail) from cleanup_error


def runtime_workflow(prefix, work, version, core_version):
    home, root = work / 'home', work / 'Example Project Пример'
    home.mkdir(mode=0o700)
    root.mkdir()
    (root / 'README.md').write_text('# Portable quota\n\nThe quota is defined by quotaLimit.\n', encoding='utf-8')
    (root / 'quota.cpp').write_text('int quotaLimit() { return 17; }\n', encoding='utf-8')
    before = {p.name: p.read_bytes() for p in root.iterdir()}
    environment = dict(HOME=str(home), PATH='', LC_ALL='C.UTF-8', TMPDIR=str(work))
    for name in ('shin-context', 'shin-observer', 'shin-ce', 'shin-usage'):
        value = json.loads(checked_run([prefix / 'bin' / name, '--version'], cwd=root, env=environment))
        require(value['version'] == (core_version if name == 'shin-context' else version), 'Binary version mismatch')
        if name in ('shin-observer', 'shin-ce'):
            require(value['core_version'] == core_version, 'Companion Core version mismatch')
    ce = prefix / 'bin/shin-ce'
    state = home / '.shin-ce'

    def call(*arguments):
        value = json.loads(checked_run([ce, *arguments], cwd=root, env=environment))
        require(value.get('ok') and value['version'] == version and value['core_version'] == core_version,
                'Extracted CE command failed or used different components')
        return value

    # Registered serving requires an already enrolled registry. Prepare the
    # supported local binding first, then own an explicit-root foreground child.
    initialized = call('init', '--no-service')
    require(initialized['local_ready'], 'Synthetic project was not locally initialized')
    service = subprocess.Popen([str(ce), 'serve', '--root', str(root), '--home', str(state)], cwd=root, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    endpoint = state / 'service/rpc.sock'
    try:
        deadline = time.monotonic() + 15
        while not endpoint.exists() and service.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        require(endpoint.exists(), 'Isolated qualification service did not start')

        for arguments in (('status', '--service'), ('query', 'quotaLimit', '--service')):
            value = call(*arguments)
            require(value['local_ready'] and not value['canonical_ready'] and
                    not value['consumer_mutated'] and not value['delivery_performed'], 'Synthetic retrieval readiness mismatch')
        require('return 17' in value['source_context'], 'Synthetic query omitted its source')
        snapshot = value['snapshot_id']
        for command in ('inspect', 'read'):
            result = call(command, 'quota.cpp', '--snapshot', snapshot, '--service')
            require(result['snapshot_id'] == snapshot, 'CE follow-up lost its snapshot binding')
            if command == 'read':
                require('return 17' in result['source_context'], 'CE read omitted the synthetic source')
        mcp_workflow(prefix, root, environment, version, core_version)
        require(before == {p.name: p.read_bytes() for p in root.iterdir()}, 'Qualification mutated its synthetic source')
    finally:
        stop_qualification_service(service, endpoint, ce, root, environment)
    # The existing PTY gate owns disposable service/project state and checks EN/RU,
    # resize, signals, terminal restoration and unchanged source/registry state.
    checked_run([sys.executable, '-B', ROOT / 'observer/tui/terminal_test.py', ce,
                 prefix / 'share/shin-context/profiles'], env=environment, cwd=work, timeout=240)


def qualification_receipt(archive, manifest, source_sha, run_id, attempt, runtime, checks):
    require(isinstance(source_sha, str) and re.fullmatch('[0-9a-f]{40}', source_sha), 'Invalid source SHA')
    require(type(run_id) is int and type(attempt) is int and
            ((run_id == 0 and attempt == 0) or (run_id > 0 and attempt > 0)), 'Invalid CI provenance')
    require(checks == CHECKS, 'Incomplete Linux qualification checks')
    require(set(runtime) == {'system', 'distribution', 'distribution_version', 'architecture', 'glibc'} and
            runtime['system'] == 'linux' and runtime['distribution'] == 'ubuntu' and
            runtime['distribution_version'] == '24.04' and runtime['architecture'] == 'x86_64' and
            isinstance(runtime['glibc'], str) and
            re.fullmatch(r'[0-9]+\.[0-9]+', runtime['glibc']) and
            tuple(map(int, runtime['glibc'].split('.'))) >= (2, 39), 'Invalid Linux runtime proof')
    return dict(schema_version='shin.linux-qualification.v1', version=manifest['version'],
                core_version=manifest['core_version'], source_sha=source_sha, run_id=run_id, attempt=attempt,
                assets=asset_hashes(archive), checks=list(checks), runtime=runtime)


def qualify(archive, source_sha, run_id, attempt):
    version, core = (ROOT / 'observer/VERSION').read_text().strip(), (ROOT / 'VERSION').read_text().strip()
    manifest = verify_archive(archive, version, core)
    assets = asset_hashes(archive)
    runtime = linux_runtime()
    with tempfile.TemporaryDirectory(prefix='shin-linux-proof-', dir='/tmp') as temporary:
        work = Path(temporary)
        extracted = work / 'relocated kit'
        extracted.mkdir()
        prefix = extract_archive(archive, extracted, manifest)
        verify_elf_dependencies(prefix)
        runtime_workflow(prefix, work, version, core)
    require(verify_archive(archive, version, core) == manifest and asset_hashes(archive) == assets,
            'Linux package changed during qualification')
    receipt = qualification_receipt(archive, manifest, source_sha, run_id, attempt, runtime, CHECKS)
    (archive.parent / 'linux-qualification.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def package(cmake, build, output, source_sha, run_id, attempt):
    linux_runtime()
    output.mkdir(parents=True, exist_ok=True)
    require(not list(output.iterdir()), 'Linux package output directory must be empty')
    version, core = (ROOT / 'observer/VERSION').read_text().strip(), (ROOT / 'VERSION').read_text().strip()
    name = stem(version)
    archive = output / (name + '.tar.gz')
    with tempfile.TemporaryDirectory(prefix='shin-linux-package-', dir='/tmp') as temporary:
        prefix = Path(temporary) / 'install prefix' / name
        environment = {k: v for k, v in os.environ.items() if k not in ('DESTDIR', 'CMAKE_INSTALL_MODE')}
        checked_run([cmake, '--install', build.resolve(), '--prefix', prefix], env=environment, timeout=90)
        (prefix / 'README-linux.txt').write_text(
            f'Shin terminal kit {version} (Core {core})\n\n'
            'Runtime: Ubuntu 24.04 x86_64, glibc 2.39 or newer; system packages\n'
            'libc6, libstdc++6, libssl3t64 (OpenSSL 3), libsqlite3-0.\n'
            'This archive provides the native CLI, local service, MCP and EN/RU terminal UI.\n'
            'Desktop/tray, automatic updates and service-manager installation are outside this kit.\n\n'
            f'sha256sum -c {name}.tar.gz.sha256\ntar -xzf {name}.tar.gz\n'
            f'./{name}/bin/shin-ce --version\n\n'
            'From a project directory, use the absolute extracted path:\n'
            f'/path/to/{name}/bin/shin-ce init\n/path/to/{name}/bin/shin-ce stats\n'
            f'/path/to/{name}/bin/shin-ce service-stop\n\n'
            'init creates private state under $HOME/.shin-ce and enrolls the current project.\n'
            'Keep the extracted directory while using that service.\n', encoding='utf-8')
        files = []
        for path in sorted(prefix.rglob('*')):
            require(not path.is_symlink() and (path.is_dir() or path.is_file()), 'Installed payload contains a link or special file')
            if path.is_file():
                files.append(dict(path=path.relative_to(prefix).as_posix(), size=path.stat().st_size,
                                  sha256=digest(path), executable=bool(path.stat().st_mode & 0o111)))
        manifest = dict(schema_version='shin.terminal-kit.v1', version=version, core_version=core,
                        platform='linux', architecture='x86_64', runtime=RUNTIME, files=files)
        (prefix / MANIFEST).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        def public_metadata(info):
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ''
            info.pax_headers = {}
            return info
        with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as kit:
            for path in [prefix, *sorted(prefix.rglob('*'))]:
                kit.add(path, arcname=name + ('/' + path.relative_to(prefix).as_posix() if path != prefix else ''),
                        recursive=False, filter=public_metadata)
    (output / (archive.name + '.sha256')).write_text(f'{digest(archive)}  {archive.name}\n', encoding='utf-8')
    return qualify(archive, source_sha, run_id, attempt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    create = commands.add_parser('package')
    create.add_argument('--cmake', default='cmake')
    create.add_argument('--build', type=Path, required=True)
    create.add_argument('--output', type=Path, required=True)
    check = commands.add_parser('qualify')
    check.add_argument('--archive', type=Path, required=True)
    for command in (create, check):
        command.add_argument('--source-sha', required=True)
        command.add_argument('--run-id', type=int, required=True)
        command.add_argument('--attempt', type=int, required=True)
    args = parser.parse_args()
    if args.command == 'package':
        receipt = package(args.cmake, args.build, args.output.resolve(), args.source_sha, args.run_id, args.attempt)
    else:
        receipt = qualify(args.archive.resolve(), args.source_sha, args.run_id, args.attempt)
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
