import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

from pinned_archive import download_pinned_archive


class PinnedArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='shin-pinned-download-')
        self.addCleanup(self.temporary.cleanup)
        self.archive = Path(self.temporary.name) / 'kit.zip'
        self.content = b'fixture archive'
        self.pin = dict(tag='v1.2.3-dev.4', archive=self.archive.name, bytes=len(self.content),
                        sha256=hashlib.sha256(self.content).hexdigest(),
                        url='https://github.com/toseek-samurai/shin-context-engine/releases/download/v1.2.3-dev.4/kit.zip')

    def test_successful_direct_download_does_not_invoke_api(self):
        calls = []
        def run(command, **options):
            calls.append(command)
            self.archive.write_bytes(self.content)
        self.assertEqual(download_pinned_archive(self.pin, self.archive, run).read_bytes(), self.content)
        self.assertEqual(len(calls), 1)
        self.assertIn('--retry-all-errors', calls[0])

    def test_transport_failure_discards_partial_data_then_uses_exact_pinned_asset(self):
        calls = []
        def run(command, **options):
            calls.append(command)
            if command[0] == '/usr/bin/curl':
                self.archive.write_bytes(b'partial')
                raise subprocess.CalledProcessError(56, command)
            self.assertFalse(self.archive.exists())
            self.assertEqual(command[:6], ['gh', 'release', 'download', self.pin['tag'], '--repo', 'toseek-samurai/shin-context-engine'])
            self.assertEqual(command[command.index('--pattern')+1], self.pin['archive'])
            self.archive.write_bytes(self.content)
        download_pinned_archive(self.pin, self.archive, run)
        self.assertEqual(len(calls), 2)
        self.assertEqual(self.archive.read_bytes(), self.content)

    def test_checksum_or_size_failure_never_switches_to_another_source(self):
        for invalid in (b'wrong', b'x' * len(self.content)):
            calls = []
            def run(command, **options):
                calls.append(command)
                self.archive.write_bytes(invalid)
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                download_pinned_archive(self.pin, self.archive, run)
            self.assertEqual(len(calls), 1)
            self.assertFalse(self.archive.exists())

    def test_api_failure_or_invalid_api_bytes_do_not_leave_a_candidate(self):
        for invalid in (None, b'wrong', b'x' * len(self.content)):
            def run(command, **options):
                if command[0] == '/usr/bin/curl' or invalid is None:
                    raise subprocess.CalledProcessError(22, command)
                self.archive.write_bytes(invalid)
            with self.subTest(invalid=invalid), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                download_pinned_archive(self.pin, self.archive, run)
            self.assertFalse(self.archive.exists())

    def test_foreign_origin_and_existing_destination_are_preserved(self):
        def forbidden(*args, **kwargs):
            self.fail('Invalid input started a download')
        for change in (dict(url=self.pin['url'].replace('https:', 'http:')),
                       dict(url=self.pin['url'].replace('github.com', 'example.test')),
                       dict(tag='v9.9.9-dev.1'), dict(archive='../escape'), dict(bytes=0)):
            with self.subTest(change=change), self.assertRaises(ValueError):
                download_pinned_archive({**self.pin, **change}, self.archive, forbidden)
        self.archive.write_bytes(b'keep')
        with self.assertRaises(ValueError):
            download_pinned_archive(self.pin, self.archive, forbidden)
        self.assertEqual(self.archive.read_bytes(), b'keep')


if __name__ == '__main__':
    unittest.main()
