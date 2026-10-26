#!/usr/bin/env python3
"""Build the stable updater envelope from the exact qualified kit bytes."""
import argparse
import hashlib
import json
import stat
from pathlib import Path
import zipfile


def package(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    name = 'shin-updater-v1-macos-arm64.zip'
    with zipfile.ZipFile(archive) as kit:
        manifests = [n for n in kit.namelist() if n.count('/') == 1 and n.endswith('/shin-updater.json')]
        assert len(manifests) == 1
        prefix = manifests[0][:-len('shin-updater.json')]
        manifest_bytes = kit.read(manifests[0])
        manifest = json.loads(manifest_bytes)
        assert manifest['schema_version'] == 'shin.updater.v1' and manifest['protocol'] == 1
        names = ['shin-updater.json', *(row['path'] for row in manifest['files'])]
        assert len(names) == len(set(names)) and 'bin/shin-updater' in names
        with zipfile.ZipFile(destination / name, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            # Explicit directories retain safe modes under a user's group-write
            # umask; extraction never needs a chmod/quarantine workaround.
            parents = {'shin-updater-v1/'}
            for path in names:
                parents.update('shin-updater-v1/' + str(parent) + '/' for parent in Path(path).parents if str(parent) != '.')
            for parent in sorted(parents):
                directory = zipfile.ZipInfo(parent)
                directory.create_system = 3
                directory.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                output.writestr(directory, b'')
            for path in names:
                assert not Path(path).is_absolute() and '..' not in Path(path).parts
                entry = kit.getinfo(prefix + path)
                payload = kit.read(entry)
                if path != 'shin-updater.json':
                    row = next(row for row in manifest['files'] if row['path'] == path)
                    assert len(payload) == row['size'] and hashlib.sha256(payload).hexdigest() == row['sha256']
                copied = zipfile.ZipInfo('shin-updater-v1/' + path, date_time=entry.date_time)
                copied.create_system = 3
                copied.external_attr = entry.external_attr
                output.writestr(copied, payload, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    digest = hashlib.sha256((destination / name).read_bytes()).hexdigest()
    (destination / (name + '.sha256')).write_text(f'{digest}  {name}\n')
    return destination / name


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    print(package(args.archive, args.destination))
