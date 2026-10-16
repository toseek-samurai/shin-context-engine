"""Admission of one explicitly pinned, immutable semantic generation."""

import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat

MAX_BYTES = 300 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
MAX_UNITS = 130000
DIMENSIONS = 384
SHA = re.compile(r"[0-9a-f]{64}")
FNV = re.compile(r"fnv1a64:[0-9a-f]{16}")
SLUG = re.compile(r"[a-z][a-z0-9-]{0,62}")
UNIT_FIELDS = {"project", "path", "start_line", "end_line", "source_sha256", "record_ids", "text_sha256"}
FILES = {"units.jsonl", "vectors.npy", "links.jsonl", "publication.json"}


class Rejected(ValueError):
    """Public errors are fixed codes, never raw source or child diagnostics."""


def require(condition, code="invalid-semantic-artifact"):
    if not condition:
        raise Rejected(code)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encode(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def decode(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate-json-key")
            result[key] = value
        return result

    def invalid(_):
        raise Rejected("nonfinite-json")

    return json.loads(data, object_pairs_hook=pairs, parse_constant=invalid)


def canonical(value):
    require(isinstance(value, str) and 0 < len(value.encode()) <= 4096 and "\0" not in value, "invalid-path")
    path = Path(value)
    require(path.is_absolute() and path != Path("/") and str(path) == value, "invalid-path")
    try:
        require(path.resolve() == path, "symlink-path")
    except RuntimeError:
        raise Rejected("cyclic-path") from None
    return path


def relative(value):
    return (isinstance(value, str) and 0 < len(value.encode()) <= 4096 and "\0" not in value and "\\" not in value
            and not PurePosixPath(value).is_absolute() and ".." not in PurePosixPath(value).parts
            and str(PurePosixPath(value)) == value and value != ".")


def read(path, limit, private=False):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and 0 <= info.st_size <= limit, "unsafe-or-excessive-file")
        if private:
            require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o600, "unsafe-private-file")
        data = stream.read(limit + 1)
        require(len(data) <= limit, "file-byte-budget")
        return data


def publication_id(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"fnv1a64:{value:016x}"


def root_identity(path):
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode), "invalid-root")
    return f"{info.st_dev}:{info.st_ino}"


class Artifact:
    def __init__(self, directory, expected_sha, route):
        import numpy as np

        directory = canonical(str(directory))
        info = directory.stat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
                and stat.S_IMODE(info.st_mode) == 0o700, "unsafe-artifact-directory")
        require(isinstance(expected_sha, str) and SHA.fullmatch(expected_sha), "invalid-artifact-pin")
        data = read(directory / "manifest.json", MAX_MANIFEST, private=True)
        require(sha(data) == expected_sha, "artifact-manifest-changed")
        manifest = decode(data)
        require(isinstance(manifest, dict) and set(manifest) == {
            "schema_version", "binding", "model", "projection", "selection", "units", "files"})
        require(manifest["schema_version"] == "shin.semantic-artifact.v1")
        require(manifest["projection"] in ("core-bound-range-v1", "core-bound-range-redacted-v2")
                and manifest["selection"] == "code-anchor-source-resolution-v1")
        binding = manifest["binding"]
        require(isinstance(binding, dict) and set(binding) == {
            "project", "root", "root_identity", "profile", "profile_sha256", "profile_config_hash",
            "snapshot_id", "publication_sha256", "generator"})
        for key in ("project", "root", "root_identity", "profile", "profile_sha256"):
            require(binding[key] == route[key], "artifact-route-mismatch")
        require(isinstance(binding["snapshot_id"], str) and FNV.fullmatch(binding["snapshot_id"]))
        require(type(manifest["units"]) is int and 0 < manifest["units"] <= MAX_UNITS)
        files = manifest["files"]
        require(isinstance(files, dict) and set(files) == FILES)
        for item in files.values():
            require(isinstance(item, dict) and set(item) == {"bytes", "sha256"})
            require(type(item["bytes"]) is int and 0 <= item["bytes"] <= MAX_BYTES)
            require(isinstance(item["sha256"], str) and SHA.fullmatch(item["sha256"]))
        require(sum(item["bytes"] for item in files.values()) + len(data) <= MAX_BYTES, "artifact-byte-budget")

        def captured(name):
            value = read(directory / name, files[name]["bytes"], private=True)
            require(len(value) == files[name]["bytes"] and sha(value) == files[name]["sha256"], "artifact-file-changed")
            return value

        publication = captured("publication.json")
        require(sha(publication) == binding["publication_sha256"] and publication_id(publication) == binding["snapshot_id"],
                "publication-binding-mismatch")
        published = decode(publication)
        require(published["profile"] == binding["profile"] and published["profile_config_hash"] == binding["profile_config_hash"]
                and published["generator_version"] == binding["generator"], "publication-profile-mismatch")
        units = [decode(line) for line in captured("units.jsonl").split(b"\n") if line]
        require(len(units) == manifest["units"])
        self.lookup, self.by_path, self.sources = {}, {}, {}
        for index, unit in enumerate(units):
            require(isinstance(unit, dict) and set(unit) == UNIT_FIELDS and unit["project"] == binding["project"])
            require(relative(unit["path"]))
            require(type(unit["start_line"]) is int and type(unit["end_line"]) is int
                    and 1 <= unit["start_line"] <= unit["end_line"] <= 100000000)
            for field in ("source_sha256", "text_sha256"):
                require(isinstance(unit[field], str) and SHA.fullmatch(unit[field]))
            ids = unit["record_ids"]
            require(isinstance(ids, list) and 0 < len(ids) <= 1024 and
                    all(isinstance(value, str) and 0 < len(value.encode()) <= 4096 for value in ids))
            require(len(set(ids)) == len(ids))
            key = unit["path"], unit["start_line"], unit["end_line"]
            require(key not in self.lookup, "duplicate-unit-range")
            self.lookup[key] = index
            self.by_path.setdefault(unit["path"], []).append(index)
            require(self.sources.setdefault(unit["path"], unit["source_sha256"]) == unit["source_sha256"], "mixed-source-generation")
        links = captured("links.jsonl")
        integrity = published["generated_file_integrity"]
        entries = [row for row in integrity["files"] if row["path"] == "docs/kb/generated/links.jsonl"]
        require(integrity["algorithm"] == "fnv1a64" and len(entries) == 1)
        require(entries[0]["bytes"] == len(links) and entries[0]["content_hash"] == publication_id(links), "link-publication-mismatch")
        self.links = {}
        for line in links.split(b"\n"):
            if not line:
                continue
            row = decode(line)
            require(isinstance(row, dict))
            if row.get("kind") in ("source-reference", "source-payload"):
                require(relative(row.get("path")) and type(row.get("line")) is int and row["line"] > 0)
                self.links.setdefault(row["path"], []).append(row)
        # Own matrix bytes, never a live mmap that can silently change generation.
        vector_bytes = captured("vectors.npy")
        stream = io.BytesIO(vector_bytes)
        require(np.lib.format.read_magic(stream) == (1, 0), "unsupported-vector-format")
        shape, fortran, dtype = np.lib.format.read_array_header_1_0(stream)
        require(shape == (len(units), DIMENSIONS) and not fortran and dtype == np.dtype("<f4"), "invalid-vector-layout")
        require(len(vector_bytes) == stream.tell() + len(units) * DIMENSIONS * 4, "vector-byte-count")
        matrix = np.frombuffer(vector_bytes, dtype=dtype, offset=stream.tell()).reshape(shape)
        for start in range(0, len(units), 2048):
            part = matrix[start:start + 2048]
            require(np.isfinite(part).all() and np.all(np.abs(np.linalg.norm(part, axis=1) - 1) < 0.001), "invalid-vector-values")
        matrix.flags.writeable = False
        self.manifest, self.units, self.matrix = manifest, units, matrix

    def admit(self, core):
        require(core["snapshot_id"] == self.manifest["binding"]["snapshot_id"], "semantic-snapshot-mismatch")
