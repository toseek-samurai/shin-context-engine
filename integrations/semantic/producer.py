"""Explicit current-publication build, bounded vector reuse and atomic activation."""

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import resource
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

from .artifact import Artifact, MAX_BYTES, SHA, canonical, decode, encode, publication_id, read, require, root_identity, sha
from . import model
from .projection import capture_publication, project_units
from .runtime import BASE_ROUTE_FIELDS, validate_base_route, verify_base_route

BATCH_SIZE = 4
MAX_SECONDS = 1800


def overlap(left, right):
    return left.is_relative_to(right) or right.is_relative_to(left)


def private_directory(path):
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == 0o700, "unsafe-producer-directory")


def save(path, data):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def optional_private(path):
    try:
        return read(path, 65536, private=True)
    except FileNotFoundError:
        return None


@contextmanager
def writer_lock(store):
    descriptor = os.open(store / "producer.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(descriptor)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1
                and stat.S_IMODE(info.st_mode) == 0o600, "unsafe-producer-lock")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            require(False, "semantic-writer-busy")
        yield
    finally:
        os.close(descriptor)


def capture_command(args, output, limit, timeout):
    process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               start_new_session=True, env={"PATH": "", "LC_ALL": "C"})
    deadline, total = time.monotonic() + timeout, 0
    try:
        while True:
            remaining = deadline - time.monotonic()
            require(remaining > 0 and select.select([process.stdout], [], [], remaining)[0], "producer-child-timeout")
            block = os.read(process.stdout.fileno(), 65536)
            if not block:
                break
            total += len(block)
            require(total <= limit, "producer-child-output-budget")
            output.write(block)
        require(process.wait(timeout=max(0.001, deadline - time.monotonic())) == 0, "producer-child-rejected")
    finally:
        if process.returncode is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        process.stdout.close()


class Producer:
    def __init__(self, config_path):
        self.config_path = canonical(str(config_path))
        self.config_bytes = read(self.config_path, 65536, private=True)
        config = decode(self.config_bytes)
        require(isinstance(config, dict) and set(config) == {
            "schema_version", "route", "exporter_binary", "exporter_sha256", "store_dir"}, "invalid-build-config")
        require(config["schema_version"] == "shin.semantic-build.v1" and set(config["route"]) == BASE_ROUTE_FIELDS,
                "invalid-build-config")
        validate_base_route(config["route"])
        require(isinstance(config["exporter_sha256"], str) and SHA.fullmatch(config["exporter_sha256"]), "invalid-exporter-pin")
        self.config, self.route = config, config["route"]
        self.exporter = canonical(config["exporter_binary"])
        self.store = canonical(config["store_dir"])
        for key in ("root", "out_dir", "model_dir"):
            require(not overlap(self.store, Path(self.route[key])), "store-overlaps-input")
        require(not overlap(self.store, Path(self.route["profile_path"]).parent), "store-overlaps-profile")
        require(not self.config_path.is_relative_to(self.store) and not self.exporter.is_relative_to(self.store)
                and not Path(self.route["core_binary"]).is_relative_to(self.store), "store-overlaps-config-or-executable")
        self.verify()

    def verify(self):
        require(read(self.config_path, 65536, private=True) == self.config_bytes, "build-config-changed")
        verify_base_route(self.route)
        require(sha(read(canonical(str(self.exporter)), 64 * 1024 * 1024)) == self.config["exporter_sha256"], "exporter-changed")

    def core_check(self):
        import io

        output = io.BytesIO()
        route = self.route
        capture_command([route["core_binary"], "--profile", route["profile"], "--profile-dir", str(Path(route["profile_path"]).parent),
                         "--root", route["root"], "--out-dir", route["out_dir"], "check"], output, 128 * 1024, 60)
        value = decode(output.getvalue())
        require(value.get("ok") is True and value.get("current") is True and value.get("command") == "check"
                and value.get("tool") == "shin-context" and value.get("profile") == route["profile"], "producer-core-not-current")

    def budget(self, started):
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * (1 if sys.platform == "darwin" else 1024)
        require(time.monotonic() - started <= MAX_SECONDS and rss <= 3 * 1024 * 1024 * 1024, "producer-resource-budget")
        return rss

    def donor(self, data, internal):
        if data is None:
            return None
        route = decode(data)
        validate_base_route(route)
        require(route.get("schema_version") == "shin.semantic-route.v1", "invalid-donor-route")
        for key in ("project", "root", "root_identity", "profile"):
            require(route[key] == self.route[key], "donor-scope-mismatch")
        directory = canonical(route["artifact_dir"])
        if internal:
            require(directory.parent == self.store / "generations" and directory.name == route["artifact_sha256"], "foreign-active-generation")
        artifact = Artifact(directory, route["artifact_sha256"], route)
        compatible = (artifact.manifest["model"] == model.MODEL and
                      route["profile_sha256"] == self.route["profile_sha256"] and route["core_sha256"] == self.route["core_sha256"])
        return artifact if compatible else None

    def redact_inputs(self, stage, units, texts, manifest):
        source, output = stage / "model-input.jsonl", stage / "model-input-redacted.jsonl"
        hashes = {row["path"]: row["content_hash"] for row in [*manifest["sources"], *manifest["source_fact_sources"]]
                  if row.get("content_hash")}
        data = b"".join(encode({"index": i, "text": text, "path": units[i]["path"],
            "start_line": units[i]["start_line"], "end_line": units[i]["end_line"],
            "source_content_hash": hashes[units[i]["path"]]}) + b"\n" for i, text in enumerate(texts))
        require(len(data) <= MAX_BYTES, "model-input-budget")
        save(source, data)
        save(output, b"")
        with output.open("wb") as stream:
            capture_command([str(self.exporter), "--redact-model-input", self.route["root"], str(source)], stream, MAX_BYTES, 120)
        rows = [decode(line) for line in read(output, MAX_BYTES, private=True).split(b"\n") if line]
        require(len(rows) == len(texts) + 1 and rows[-1] == {"type": "complete", "records": len(texts)}, "incomplete-redaction")
        count = 0
        for i, row in enumerate(rows[:-1]):
            require(set(row) == {"index", "text", "redacted"} and row["index"] == i
                    and isinstance(row["text"], str) and type(row["redacted"]) is bool, "invalid-redaction")
            require(row["text"] == texts[i] or row["redacted"], "redaction-altered-safe-input")
            texts[i] = row["text"]
            units[i]["text_sha256"] = sha(row["text"].encode())
            count += row["redacted"]
        source.unlink()
        output.unlink()
        return count

    def build(self, reuse_route=None):
        started = time.monotonic()
        self.verify()
        self.store.mkdir(mode=0o700, exist_ok=True)
        private_directory(self.store)
        store_identity = root_identity(self.store)
        with writer_lock(self.store):
            generations = self.store / "generations"
            canonical(str(generations))
            generations.mkdir(mode=0o700, exist_ok=True)
            private_directory(generations)
            current = self.store / "current-route.json"
            previous = optional_private(current)
            require(not (previous is not None and reuse_route is not None), "explicit-reuse-only-on-initial-build")
            donor_data = previous if reuse_route is None else read(canonical(str(reuse_route)), 65536, private=True)
            prior = self.donor(donor_data, internal=reuse_route is None)
            self.core_check()
            publication, manifest, files = capture_publication(self.route)
            stage = Path(tempfile.mkdtemp(prefix=".stage-", dir=self.store))
            try:
                export = stage / "source-ranges.jsonl"
                save(export, b"")
                with export.open("wb") as stream:
                    capture_command([str(self.exporter), self.route["root"], self.route["out_dir"]], stream, MAX_BYTES, 120)
                units, texts, counts = project_units(self.route, publication, files, read(export, MAX_BYTES, private=True))
                export.unlink()
                counts["fully_redacted_units"] = self.redact_inputs(stage, units, texts, manifest)
                self.budget(started)
                import numpy as np
                vectors = np.empty((len(units), 384), dtype=np.float32)
                exact, by_text = {}, {}
                if prior is not None:
                    for index, unit in enumerate(prior.units):
                        exact[sha(encode(unit))] = index
                        by_text.setdefault(unit["text_sha256"], index)
                missing, reused = [], 0
                for index, unit in enumerate(units):
                    donor_index = exact.get(sha(encode(unit)), by_text.get(unit["text_sha256"]))
                    if donor_index is None:
                        missing.append(index)
                    else:
                        vectors[index] = prior.matrix[donor_index]
                        reused += 1
                encoder = model.load(Path(self.route["model_dir"]), model.MODEL) if missing else None
                for start in range(0, len(missing), BATCH_SIZE):
                    indexes = missing[start:start + BATCH_SIZE]
                    values = list(encoder.embed([model.MODEL["passage_prefix"] + texts[i] for i in indexes], batch_size=BATCH_SIZE))
                    require(len(values) == len(indexes), "embedding-count-mismatch")
                    for index, value in zip(indexes, values):
                        vector = np.asarray(value, dtype=np.float32)
                        require(vector.shape == (384,) and np.isfinite(vector).all()
                                and abs(float(np.linalg.norm(vector)) - 1) < 0.001, "invalid-document-vector")
                        vectors[index] = vector
                    self.budget(started)
                save(stage / "units.jsonl", b"".join(encode(unit) + b"\n" for unit in units))
                save(stage / "publication.json", publication)
                save(stage / "links.jsonl", files["links.jsonl"])
                vector_file = stage / "vectors.npy"
                save(vector_file, b"")
                with vector_file.open("wb") as stream:
                    np.save(stream, vectors, allow_pickle=False)
                    stream.flush()
                    os.fsync(stream.fileno())
                binding = {key: self.route[key] for key in ("project", "root", "root_identity", "profile", "profile_sha256")}
                binding.update(snapshot_id=publication_id(publication), publication_sha256=sha(publication),
                               profile_config_hash=manifest["profile_config_hash"], generator=manifest["generator_version"])
                descriptor = {"schema_version": "shin.semantic-artifact.v1", "binding": binding, "model": model.MODEL,
                    "projection": "core-bound-range-redacted-v2", "selection": "code-anchor-source-resolution-v1", "units": len(units),
                    "files": {name: {"bytes": (stage / name).stat().st_size, "sha256": sha(read(stage / name, MAX_BYTES, private=True))}
                              for name in ("units.jsonl", "publication.json", "links.jsonl", "vectors.npy")}}
                data = encode(descriptor) + b"\n"
                save(stage / "manifest.json", data)
                identity = sha(data)
                target = generations / identity
                route = {"schema_version": "shin.semantic-route.v1", **self.route,
                         "artifact_dir": str(target), "artifact_sha256": identity}
                Artifact(stage, identity, route)
                del vectors, texts, prior
                self.verify()
                self.core_check()
                require(read(Path(self.route["out_dir"]) / "docs/kb/generated/manifest.json", 8 * 1024 * 1024) == publication,
                        "publication-changed-during-build")
                require(root_identity(canonical(str(self.store))) == store_identity, "store-identity-changed")
                require(optional_private(current) == previous, "active-route-changed")
                self.budget(started)
                if target.exists():
                    Artifact(canonical(str(target)), identity, route)
                    require(read(target / "manifest.json", 1024 * 1024, private=True) == data, "generation-collision")
                else:
                    sync_directory(stage)
                    os.rename(stage, target)
                    sync_directory(generations)
                route_bytes = encode(route) + b"\n"
                activated = previous != route_bytes
                result = {"schema_version": "shin.semantic-build-result.v1", "ok": True, "project": self.route["project"],
                    "snapshot_id": binding["snapshot_id"], "artifact_sha256": identity, "route": str(current),
                    "activated": activated, "units": len(units), "vectors_reused": reused, "vectors_computed": len(missing),
                    "donor_compatible": donor_data is not None and bool(exact), "projection": counts,
                    "artifact_bytes": sum(row["bytes"] for row in descriptor["files"].values()) + len(data),
                    "peak_rss_bytes": self.budget(started), "core_rebuilt": False, "source_mutated": False, "index_mutated": False}
                if activated:
                    temporary = self.store / (".route-" + os.urandom(8).hex())
                    try:
                        save(temporary, route_bytes)
                        require(optional_private(current) == previous, "active-route-changed")
                        os.replace(temporary, current)
                        try:
                            sync_directory(self.store)
                        except OSError:
                            result.update(ok=False, error={"code": "publication-durability-uncertain"})
                    finally:
                        temporary.unlink(missing_ok=True)
                result["seconds"] = round(time.monotonic() - started, 3)
                return result
            finally:
                if stage.exists():
                    shutil.rmtree(stage)
