"""Portable Linux TAR rejection and exact qualification receipt checks."""
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch

import linux_package as package

VERSION = '2.0.5-dev.91'
CORE = '2.0.5-dev.13'
SHA = 'b' * 40


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='shin-linux-package-test-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.prefix = package.stem(VERSION)
        self.archive = self.directory / (self.prefix + '.tar.gz')
        self.payload = {path: b'independent synthetic payload\n' for path in package.REQUIRED_FILES}
        self.payload['share/shin-context/VERSION'] = (CORE + '\n').encode()
        self.payload['share/shin-observer/VERSION'] = (VERSION + '\n').encode()
        self.manifest = dict(schema_version='shin.terminal-kit.v1', version=VERSION, core_version=CORE,
                             platform='linux', architecture='x86_64', runtime=copy.deepcopy(package.RUNTIME),
                             files=[dict(path=path, size=len(value), sha256=hashlib.sha256(value).hexdigest(),
                                         executable=path.startswith('bin/')) for path, value in sorted(self.payload.items())])

    def write(self, *, extras=(), omit=(), changed=None, manifest=None, modes=None):
        manifest = self.manifest if manifest is None else manifest
        with tarfile.open(self.archive, 'w:gz') as kit:
            for path, value in [(package.MANIFEST, json.dumps(manifest).encode()), *self.payload.items()]:
                if path in omit:
                    continue
                value = (changed or {}).get(path, value)
                entry = tarfile.TarInfo(self.prefix + '/' + path)
                entry.size = len(value)
                entry.mode = (modes or {}).get(path, 0o755 if path.startswith('bin/') else 0o644)
                kit.addfile(entry, io.BytesIO(value))
            for info, value in extras:
                kit.addfile(info, io.BytesIO(value) if info.isfile() else None)
        self.checksum = self.archive.with_name(self.archive.name + '.sha256')
        self.checksum.write_text(f'{package.digest(self.archive)}  {self.archive.name}\n')

    def verify(self):
        return package.verify_archive(self.archive, VERSION, CORE)

    def test_exact_inventory_versions_modes_and_safe_manual_extraction(self):
        self.write()
        self.assertEqual(self.verify(), self.manifest)
        destination = self.directory / 'relocated kit'
        destination.mkdir()
        extracted = package.extract_archive(self.archive, destination, self.manifest)
        self.assertEqual({p.relative_to(extracted).as_posix() for p in extracted.rglob('*') if p.is_file()},
                         set(self.payload) | {package.MANIFEST})
        self.assertEqual((extracted / 'bin/shin-ce').stat().st_mode & 0o777, 0o755)
        self.assertEqual((extracted / 'share/shin-context/LICENSE').stat().st_mode & 0o777, 0o644)

    def test_changed_missing_or_unlisted_payload_fails(self):
        extra = tarfile.TarInfo(self.prefix + '/unlisted.txt')
        extra.size = 3
        for change in (dict(changed={'bin/shin-ce': b'tampered'}), dict(omit={'bin/shin-ce'}),
                       dict(omit={'share/shin-context/NOTICE'}), dict(extras=[(extra, b'new')])):
            with self.subTest(change=change):
                self.write(**change)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_same_size_tampering_fails_hash_check(self):
        original = self.payload['bin/shin-ce']
        self.write(changed={'bin/shin-ce': b'x' * len(original)})
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.verify()

    def test_duplicate_and_noncanonical_paths_are_rejected(self):
        for path in ('bin/shin-ce', '../outside', '/absolute', 'bin/../outside', 'bin//extra',
                     './extra', 'bin\\extra', 'C:extra', 'line\nextra'):
            with self.subTest(path=path):
                entry = tarfile.TarInfo(self.prefix + '/' + path)
                entry.size = 1
                self.write(extras=[(entry, b'x')])
                with self.assertRaises(ValueError):
                    self.verify()
        entry = tarfile.TarInfo('wrong-root/extra')
        entry.size = 1
        self.write(extras=[(entry, b'x')])
        with self.assertRaisesRegex(ValueError, 'root'):
            self.verify()

    def test_links_and_special_members_are_rejected_before_extraction(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE, tarfile.BLKTYPE):
            with self.subTest(kind=kind):
                entry = tarfile.TarInfo(self.prefix + '/extra')
                entry.type = kind
                entry.linkname = 'bin/shin-ce'
                self.write(extras=[(entry, b'')])
                with self.assertRaisesRegex(ValueError, 'special TAR'):
                    self.verify()

    def test_identity_and_runtime_floor_cannot_change(self):
        for field, value in [('schema_version', 'shin.local-kit.v1'), ('version', '2.0.5-dev.90'),
                             ('core_version', '2.0.5-dev.12'), ('platform', 'darwin'),
                             ('architecture', 'aarch64'), ('runtime', dict(package.RUNTIME, glibc_minimum='2.17'))]:
            with self.subTest(field=field):
                manifest = copy.deepcopy(self.manifest)
                manifest[field] = value
                self.write(manifest=manifest)
                with self.assertRaisesRegex(ValueError, 'identity or runtime'):
                    self.verify()

    def test_manifest_metadata_and_duplicate_inventory_are_rejected(self):
        for field, value in [('path', '../bad'), ('size', True), ('size', -1), ('sha256', 'bad'),
                             ('executable', 'yes'), ('executable', False)]:
            with self.subTest(field=field, value=value):
                manifest = copy.deepcopy(self.manifest)
                manifest['files'][0][field] = value
                self.write(manifest=manifest)
                with self.assertRaises(ValueError):
                    self.verify()
        manifest = copy.deepcopy(self.manifest)
        manifest['files'].append(manifest['files'][0])
        self.write(manifest=manifest)
        with self.assertRaisesRegex(ValueError, 'Duplicate Linux inventory'):
            self.verify()

    def test_payload_bounds_are_enforced(self):
        self.write()
        for bound, value in [('MAX_MEMBERS', 2), ('MAX_FILES', 2), ('MAX_FILE', 2),
                             ('MAX_PAYLOAD', 2), ('MAX_ARCHIVE', 2)]:
            with self.subTest(bound=bound), patch.object(package, bound, value):
                with self.assertRaises(ValueError):
                    self.verify()

    def test_runtime_binary_must_be_executable_even_with_matching_inventory(self):
        manifest = copy.deepcopy(self.manifest)
        next(r for r in manifest['files'] if r['path'] == 'bin/shin-ce')['executable'] = False
        self.write(manifest=manifest, modes={'bin/shin-ce': 0o644})
        with self.assertRaisesRegex(ValueError, 'must be executable'):
            self.verify()

    def test_hidden_tar_payload_and_oversized_extension_header_fail(self):
        self.write()
        raw = gzip.decompress(self.archive.read_bytes())
        # A second TAR after the first EOF must not hide unlisted payload.
        self.archive.write_bytes(gzip.compress(raw + raw))
        self.checksum.write_text(f'{package.digest(self.archive)}  {self.archive.name}\n')
        with self.assertRaisesRegex(ValueError, 'after Linux TAR EOF'):
            self.verify()
        header = tarfile.TarInfo('pax metadata')
        header.type = tarfile.XHDTYPE
        header.size = 16385
        self.archive.write_bytes(gzip.compress(header.tobuf() + bytes(17408)))
        self.checksum.write_text(f'{package.digest(self.archive)}  {self.archive.name}\n')
        with self.assertRaisesRegex(ValueError, 'extended metadata'):
            self.verify()

    def test_checksum_is_mandatory_and_bound_to_the_final_archive(self):
        self.write()
        for value in ('0' * 64 + '  ' + self.archive.name + '\n', package.digest(self.archive) + '  renamed.tar.gz\n'):
            self.checksum.write_text(value)
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                self.verify()
        self.checksum.unlink()
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.verify()

    def test_installed_component_version_files_are_checked(self):
        self.payload['share/shin-context/VERSION'] = b'2.0.5-dev.12\n'
        row = next(r for r in self.manifest['files'] if r['path'] == 'share/shin-context/VERSION')
        row.update(size=len(self.payload[row['path']]), sha256=hashlib.sha256(self.payload[row['path']]).hexdigest())
        self.write()
        with self.assertRaisesRegex(ValueError, 'Installed component VERSION'):
            self.verify()

    def test_receipt_is_bound_to_final_assets_source_attempt_and_checks(self):
        self.write()
        runtime = dict(system='linux', distribution='ubuntu', distribution_version='24.04',
                       architecture='x86_64', glibc='2.39')
        receipt = package.qualification_receipt(self.archive, self.manifest, SHA, 123, 2, runtime, package.CHECKS)
        self.assertEqual(receipt, dict(schema_version='shin.linux-qualification.v1', version=VERSION, core_version=CORE,
                         source_sha=SHA, run_id=123, attempt=2, assets=package.asset_hashes(self.archive),
                         checks=package.CHECKS, runtime=runtime))
        for kwargs in (dict(source_sha='bad'), dict(run_id=-1), dict(attempt=0), dict(run_id=True),
                       dict(checks=package.CHECKS[:-1]), dict(checks=package.CHECKS + ['extra']),
                       dict(runtime=dict(runtime, glibc='2.38')), dict(runtime=dict(runtime, private_path='/tmp/fixture'))):
            with self.subTest(kwargs=kwargs):
                arguments = dict(source_sha=SHA, run_id=123, attempt=2, runtime=runtime, checks=package.CHECKS)
                arguments.update(kwargs)
                with self.assertRaises(ValueError):
                    package.qualification_receipt(self.archive, self.manifest, **arguments)
        local = package.qualification_receipt(self.archive, self.manifest, SHA, 0, 0, runtime, package.CHECKS)
        self.assertEqual((local['run_id'], local['attempt']), (0, 0))


class DependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='shin-linux-elf-test-')
        self.addCleanup(temporary.cleanup)
        self.prefix = Path(temporary.name)
        (self.prefix / 'bin').mkdir()
        header = bytearray(64)
        header[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', header, 16, 3, 62)
        for name in package.PROGRAMS:
            (self.prefix / 'bin' / name).write_bytes(header)

    def verify(self, dependencies='/lib64/ld-linux-x86-64.so.2 (0x1234)', dynamic='(NEEDED) libc.so.6'):
        def run(arguments, **_):
            return dynamic if arguments[0] == '/usr/bin/readelf' else dependencies
        with patch.object(package, 'checked_run', side_effect=run), patch.object(Path, 'is_file', return_value=True):
            package.verify_elf_dependencies(self.prefix)

    def test_all_five_images_need_system_resolution(self):
        self.verify()
        for dependencies in ('libcrypto.so.3 => not found',
                             'libcrypto.so.3 => /tmp/developer/libcrypto.so.3 (0x1234)',
                             'statically linked'):
            with self.subTest(dependencies=dependencies), self.assertRaises(ValueError):
                self.verify(dependencies=dependencies)

    def test_developer_search_prefix_or_wrong_elf_machine_is_rejected(self):
        for tag in ('RPATH', 'RUNPATH'):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, 'search prefix'):
                self.verify(dynamic=f'({tag}) /tmp/developer/lib')
        executable = self.prefix / 'bin/shin-context'
        header = bytearray(executable.read_bytes())
        struct.pack_into('<H', header, 18, 183)
        executable.write_bytes(header)
        with self.assertRaisesRegex(ValueError, 'x86_64 ELF'):
            self.verify()


class ServiceCleanupTests(unittest.TestCase):
    def service(self, code=0, errors=b''):
        service = Mock(returncode=code)
        service.poll.return_value = code
        service.communicate.return_value = (b'', errors)
        return service

    def test_startup_failure_keeps_primary_error_and_bounded_service_evidence(self):
        service = self.service(2, b'startup error ' + b'x' * 5000)
        endpoint = Mock()
        endpoint.exists.return_value = False
        primary = ValueError('Isolated qualification service did not start')
        with patch.object(package, 'kill_group') as kill:
            with self.assertRaises(ValueError) as caught:
                try:
                    raise primary
                finally:
                    package.stop_qualification_service(service, endpoint, Path('shin-ce'), Path('.'), {})
        self.assertIs(caught.exception, primary)
        evidence = json.loads(primary.__notes__[0].split(': ', 1)[1])
        self.assertEqual(evidence['exit_code'], 2)
        self.assertEqual(len(evidence['stderr_tail']), 2000)
        self.assertFalse(evidence['endpoint_exists'])
        kill.assert_called_once_with(service)

    def test_cleanup_failure_without_primary_still_fails_with_exit_evidence(self):
        service = self.service(2, b'native startup failed')
        endpoint = Mock()
        endpoint.exists.return_value = False
        with patch.object(package, 'kill_group'), self.assertRaisesRegex(ValueError, '"exit_code": 2'):
            package.stop_qualification_service(service, endpoint, Path('shin-ce'), Path('.'), {})

    def test_successful_stop_requires_clean_exit_and_absent_endpoint(self):
        service = self.service()
        endpoint = Mock()
        endpoint.exists.side_effect = [True, False]
        with patch.object(package, 'kill_group') as kill, \
                patch.object(package, 'checked_run', return_value='{"ok":true}') as stop:
            package.stop_qualification_service(service, endpoint, Path('shin-ce'), Path('.'), {})
        stop.assert_called_once()
        self.assertEqual(stop.call_args.args[0][-1], 'service-stop')
        service.communicate.assert_called_once_with(timeout=15)
        kill.assert_called_once_with(service)


if __name__ == '__main__':
    unittest.main()
