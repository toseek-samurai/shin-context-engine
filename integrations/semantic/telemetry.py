"""Bounded local query metadata, never retrieval evidence or agent evaluation."""

from contextlib import contextmanager
import fcntl
import math
import os
from pathlib import Path
import stat
import time
import uuid

from .artifact import FNV, SHA, SLUG, canonical, decode, encode, read, require, sha

MAX_RECORDS = 256
MAX_BYTES = 1024 * 1024
MAX_RECORD_BYTES = 4096
PHASES = {"route-admission", "core-brief", "optional-runtime", "artifact-admission",
          "model-load", "query-embedding", "selection", "response-validation", "projection"}
STATUSES = {"available", "not-needed-exact-or-owner", "artifact-unavailable-lexical-only",
            "model-unavailable-lexical-only", "runtime-unavailable-lexical-only"}
REASONS = {"core-admission-rejected", "core-currentness-rejected", "core-response-mismatch",
           "semantic-snapshot-mismatch", "route-config-changed", "root-identity-changed",
           "profile-changed", "core-binary-changed", "model-runtime-mismatch", "model-manifest-changed",
           "model-file-changed", "model-registration-conflict", "semantic-memory-budget",
           "semantic-output-budget", "agent-query-output-budget", "invalid-query"}
FIELDS = {"attempt_id", "observed_at_unix_ms", "elapsed_ns", "phase_ns", "failed_phase",
          "route_key", "project", "profile", "core_sha256", "profile_sha256", "artifact_sha256",
          "snapshot_id", "ok", "reason", "semantic_status", "projection", "response_bytes",
          "native_records", "navigation_candidates", "model_state"}


class Trace:
    def __init__(self):
        self.started = time.perf_counter_ns()
        self.observed_at = time.time_ns() // 1000000
        self.phases = {}
        self.failed_phase = None
        self.client_files_sha256 = None
        try:
            root = Path(__file__).resolve().parents[2]
            files = [root / "tools/shin.py", root / "tools/shin_semantic.py",
                     *sorted((root / "integrations/semantic").glob("*.py"))]
            require(len(files) <= 32, "query-metrics-code-budget")
            self.client_files_sha256 = sha(encode({str(p.relative_to(root)): sha(read(p, 1024 * 1024)) for p in files}))
        except Exception:
            pass  # An unavailable observed code identity cannot block retrieval.

    @contextmanager
    def phase(self, name):
        started = time.perf_counter_ns()
        try:
            yield
        except BaseException:
            self.failed_phase = name
            raise
        finally:
            self.phases[name] = self.phases.get(name, 0) + time.perf_counter_ns() - started

    def record(self, config, raw, result, response_bytes, projection):
        elapsed = time.perf_counter_ns() - self.started
        core = raw.get("core", {}) if raw else {}
        navigation = raw.get("navigation") if raw else None
        reason = result.get("error", {}).get("code") if result.get("ok") is not True else None
        return {"attempt_id": uuid.uuid4().hex, "observed_at_unix_ms": self.observed_at,
                "client_files_sha256": self.client_files_sha256,
                "elapsed_ns": elapsed, "phase_ns": dict(self.phases), "failed_phase": self.failed_phase,
                "route_key": sha(encode({key: config[key] for key in ("root", "root_identity", "profile", "out_dir")})),
                "project": config["project"], "profile": config["profile"], "core_sha256": config["core_sha256"],
                "profile_sha256": config["profile_sha256"], "artifact_sha256": raw.get("artifact_sha256") if raw else None,
                "snapshot_id": core.get("snapshot_id"), "ok": result.get("ok") is True,
                "reason": None if reason is None else reason if reason in REASONS else "query-rejected",
                "semantic_status": raw.get("semantic_status") if raw else None, "projection": projection,
                "model_state": ("loaded-this-query" if "model-load" in self.phases else "reused")
                    if raw and raw.get("semantic_status") == "available" else "not-used-or-incomplete",
                "response_bytes": response_bytes, "native_records": len(core["evidence"]) if "evidence" in core else None,
                "navigation_candidates": len(navigation["candidates"]) if navigation else None}


def integer(value):
    return type(value) is int and 0 <= value <= 2**63 - 1


def validate_record(row):
    require(isinstance(row, dict) and FIELDS <= set(row) <= FIELDS | {"client_files_sha256"}, "invalid-query-metrics")
    require(row.get("client_files_sha256") is None or isinstance(row["client_files_sha256"], str)
            and SHA.fullmatch(row["client_files_sha256"]), "invalid-query-metrics")
    require(isinstance(row["attempt_id"], str) and len(row["attempt_id"]) == 32
            and all(c in "0123456789abcdef" for c in row["attempt_id"]), "invalid-query-metrics")
    for field in ("observed_at_unix_ms", "elapsed_ns", "response_bytes"):
        require(integer(row[field]), "invalid-query-metrics")
    for field in ("native_records", "navigation_candidates"):
        require(row[field] is None or integer(row[field]), "invalid-query-metrics")
    for field in ("project", "profile"):
        require(isinstance(row[field], str) and SLUG.fullmatch(row[field]), "invalid-query-metrics")
    for field in ("route_key", "core_sha256", "profile_sha256"):
        require(isinstance(row[field], str) and SHA.fullmatch(row[field]), "invalid-query-metrics")
    require(row["artifact_sha256"] is None or isinstance(row["artifact_sha256"], str)
            and SHA.fullmatch(row["artifact_sha256"]), "invalid-query-metrics")
    require(row["snapshot_id"] is None or isinstance(row["snapshot_id"], str)
            and FNV.fullmatch(row["snapshot_id"]), "invalid-query-metrics")
    require(type(row["ok"]) is bool and row["projection"] in ("compact", "audit"), "invalid-query-metrics")
    require(row["semantic_status"] is None or row["semantic_status"] in STATUSES, "invalid-query-metrics")
    require(row["model_state"] in ("loaded-this-query", "reused", "not-used-or-incomplete"), "invalid-query-metrics")
    require(row["reason"] is None or row["reason"] in REASONS | {"query-rejected"}, "invalid-query-metrics")
    require(row["ok"] == (row["reason"] is None), "invalid-query-metrics")
    require(row["failed_phase"] is None or row["failed_phase"] in PHASES, "invalid-query-metrics")
    phases = row["phase_ns"]
    require(isinstance(phases, dict) and set(phases) <= PHASES and all(integer(v) for v in phases.values())
            and sum(phases.values()) <= row["elapsed_ns"], "invalid-query-metrics")
    require(len(encode(row)) <= MAX_RECORD_BYTES, "query-metrics-byte-budget")


def _file(fd):
    info = os.fstat(fd)
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == 0o600, "unsafe-query-metrics-file")


@contextmanager
def directory(path):
    path = canonical(str(path))
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700, "unsafe-query-metrics-directory")
        yield fd
    finally:
        os.close(fd)


def _read(fd):
    child = os.open("journal.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=fd)
    with os.fdopen(child, "rb") as stream:
        _file(stream.fileno())
        require(os.fstat(stream.fileno()).st_size <= MAX_BYTES, "query-metrics-byte-budget")
        data = stream.read(MAX_BYTES + 1)
    require(len(data) <= MAX_BYTES, "query-metrics-byte-budget")
    journal = decode(data)
    require(isinstance(journal, dict) and set(journal) == {"schema_version", "epoch", "appended", "evicted", "records"}
            and journal["schema_version"] == "shin.local-query-journal.v1", "invalid-query-journal")
    require(isinstance(journal["epoch"], str) and len(journal["epoch"]) == 32
            and all(c in "0123456789abcdef" for c in journal["epoch"]), "invalid-query-journal")
    require(integer(journal["appended"]) and integer(journal["evicted"]), "invalid-query-journal")
    records = journal["records"]
    require(isinstance(records, list) and len(records) <= MAX_RECORDS
            and journal["appended"] == journal["evicted"] + len(records), "invalid-query-journal")
    for row in records:
        validate_record(row)
    require(len({row["attempt_id"] for row in records}) == len(records), "invalid-query-journal")
    return journal


def _replace(fd, value):
    data = encode(value) + b"\n"
    require(len(data) <= MAX_BYTES, "query-metrics-byte-budget")
    # A single fixed staging name bounds crash residue. Never adopt another file.
    name = "journal.pending"
    child = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, 0o600, dir_fd=fd)
    with os.fdopen(child, "wb") as stream:
        _file(stream.fileno())
        stream.truncate(0)
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(name, "journal.json", src_dir_fd=fd, dst_dir_fd=fd)
    os.fsync(fd)


def initialize(path):
    path = canonical(str(path))
    path.mkdir(mode=0o700)  # Explicit new home only; never reset an existing journal.
    with directory(path) as fd:
        lock = os.open("writer.lock", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600, dir_fd=fd)
        os.close(lock)
        _replace(fd, {"schema_version": "shin.local-query-journal.v1", "epoch": uuid.uuid4().hex,
                      "appended": 0, "evicted": 0, "records": []})


def append(path, row, config, config_path):
    validate_record(row)
    path = canonical(str(path))
    protected = [config[key] for key in ("root", "out_dir", "profile_path", "core_binary", "model_dir", "artifact_dir")]
    protected.append(str(Path(config["profile_path"]).parent))
    protected.append(str(config_path))
    for value in protected:
        other = canonical(value)
        require(not path.is_relative_to(other) and not other.is_relative_to(path), "query-metrics-route-overlap")
    with directory(path) as fd:
        lock = os.open("writer.lock", os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=fd)
        try:
            _file(lock)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            journal = _read(fd)
            require(all(r["attempt_id"] != row["attempt_id"] for r in journal["records"]), "duplicate-query-attempt")
            journal["records"].append(row)
            journal["appended"] += 1
            if len(journal["records"]) > MAX_RECORDS:
                journal["records"].pop(0)
                journal["evicted"] += 1
            _replace(fd, journal)
        finally:
            os.close(lock)


def save(path, trace, session, raw, result, response_bytes, projection):
    """No exception text, no retry after a possibly committed directory fsync."""
    try:
        require(session is not None, "unbound-query-route")
        row = trace.record(session.config, raw, result, response_bytes, projection)
        append(path, row, session.config, session.path)
    except Exception:
        return {"schema_version": "shin.query-metrics-warning.v1", "code": "query-metrics-not-confirmed",
                "retrieval_changed": False}
    return None


def report(path, *, recent_limit=20):
    require(type(recent_limit) is int and 1 <= recent_limit <= MAX_RECORDS, "invalid-query-metrics-limit")
    with directory(path) as fd:
        journal = _read(fd)
    cohorts = {}
    for row in journal["records"]:
        fields = ("route_key", "project", "profile", "client_files_sha256", "core_sha256", "profile_sha256", "artifact_sha256",
                  "snapshot_id", "semantic_status", "model_state", "projection", "ok", "reason")
        key = tuple(row.get(k) for k in fields)
        cohorts.setdefault(key, []).append(row)
    summaries = []
    for key, rows in cohorts.items():
        durations = sorted(row["elapsed_ns"] for row in rows)
        summaries.append({**dict(zip(fields, key)), "count": len(rows),
            "p50_ns": durations[math.ceil(len(rows) * 0.50) - 1], "p95_ns": durations[math.ceil(len(rows) * 0.95) - 1],
            "phase_samples": {name: {"count": len(values), "total_ns": sum(values)}
                for name in sorted(PHASES) if (values := [row["phase_ns"][name] for row in rows if name in row["phase_ns"]])}})
    return {"schema_version": "shin.local-query-metrics.v1", "scope": "retained-recorded-completed-invocations",
            "epoch": journal["epoch"], "appended": journal["appended"], "evicted": journal["evicted"],
            "retained": len(journal["records"]), "unrecorded_attempts": None, "agent_usefulness": None,
            "latency_boundary": "instrumented-call-excluding-process-startup-journal-write-and-stdout",
            "percentile_method": "nearest-rank-of-retained-observations-not-an-slo", "cohorts": summaries,
            "recent": journal["records"][-recent_limit:]}
