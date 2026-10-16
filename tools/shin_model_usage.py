#!/usr/bin/env python3
"""Opt-in Codeburn metadata import; independent of retrieval and the read-only UI."""

import argparse
from datetime import date, datetime, timedelta, timezone
import fcntl
import hashlib
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from shin import load_semantic

load_semantic()
from shin_semantic.artifact import canonical, decode, encode, require
from shin_semantic.telemetry import directory
from shin_portfolio import private_file, SLUG

CLIENTS = ("codex", "hermes", "opencode")
CODEBURN_VERSION = "0.9.24"
MAX_RAW = 64 * 1024 * 1024
MAX_HISTORY = 2 * 1024 * 1024
MAX_BUCKETS = 4000
FIELDS = {"input_tokens": "inputTokens", "output_tokens": "outputTokens",
          "cache_read_tokens": "cacheReadTokens", "cache_write_tokens": "cacheWriteTokens",
          "reasoning_tokens": "reasoningTokens"}
COUNTS = (*FIELDS, "records", "supplementary_records", "manual_binding_records")
MODEL = re.compile(r"[a-zA-Z0-9_.:/@+\[\]()-]{1,120}\Z")


def integer(value):
    require(type(value) is int and 0 <= value <= 2**53 - 1, "invalid-usage-number")
    return value


def read_private(path, maximum):
    path = canonical(str(path))
    with directory(path.parent) as parent:
        fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        with os.fdopen(fd, "rb") as stream:
            private_file(stream.fileno())
            require(os.fstat(stream.fileno()).st_size <= maximum, "usage-input-budget")
            data = stream.read(maximum + 1)
    require(len(data) <= maximum, "usage-input-budget")
    return decode(data)


def config(path):
    value = read_private(path, 65536)
    require(set(value) == {"schema_version", "codeburn", "projects", "session_bindings"}
            and value["schema_version"] == "shin.model-usage-config.v1", "invalid-usage-config")
    require(isinstance(value["codeburn"], str) and Path(value["codeburn"]).is_absolute(), "usage-binary-required")
    require(isinstance(value["projects"], list) and 1 <= len(value["projects"]) <= 24, "usage-project-budget")
    ids, paths = set(), set()
    for p in value["projects"]:
        require(set(p) == {"id", "name", "paths"} and isinstance(p["id"], str) and SLUG.fullmatch(p["id"])
                and p["id"] != "all" and p["id"] not in ids, "invalid-usage-project")
        ids.add(p["id"])
        require(isinstance(p["name"], str) and 1 <= len(p["name"]) <= 80
                and not any(ord(c) < 32 for c in p["name"]), "invalid-usage-name")
        require(isinstance(p["paths"], list) and len(p["paths"]) <= 24, "usage-path-budget")
        for root in p["paths"]:
            require(isinstance(root, str) and Path(root).is_absolute() and root not in paths,
                    "ambiguous-usage-project")
            paths.add(root)
    require(isinstance(value["session_bindings"], list) and len(value["session_bindings"]) <= 64, "usage-binding-budget")
    seen = set()
    for b in value["session_bindings"]:
        require(set(b) == {"client", "session_id", "project", "from", "to"} and b["client"] in {"codex", "opencode"} and b["project"] in ids
                and isinstance(b["session_id"], str) and 1 <= len(b["session_id"]) <= 160,
                "invalid-usage-binding")
        begin, end = instant(b["from"]), instant(b["to"])
        require(begin < end, "invalid-usage-binding-interval")
        for client, session, first, last in seen:
            require((client, session) != (b["client"], b["session_id"]) or end <= first or begin >= last,
                    "ambiguous-usage-binding")
        seen.add((b["client"], b["session_id"], begin, end))
    return value


def instant(value):
    stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    require(stamp.tzinfo is not None, "usage-timezone-required")
    return stamp.astimezone(timezone.utc)


def normalize(raw, cfg, client, start, end):
    require(raw.get("schema") == "codeburn.export.v2" and isinstance(raw.get("records"), list)
            and len(raw["records"]) <= 200000, "unsupported-codeburn-export")
    paths = {root: p["id"] for p in cfg["projects"] for root in p["paths"]}
    bindings = [(b, instant(b["from"]), instant(b["to"])) for b in cfg["session_bindings"]]
    buckets, unassigned, outside = {}, 0, 0
    for record in raw["records"]:
        require(record.get("provider") == client, "usage-client-mismatch")
        matching = [(b, first, last) for b, first, last in bindings
                    if b["client"] == client and b["session_id"] == record.get("sessionId")]
        stamp = instant(record["timestamp"]) if matching or record.get("project") in paths else None
        manual = next((b["project"] for b, first, last in matching if first <= stamp < last), None)
        project = manual or paths.get(record.get("project"))
        if project is None:
            unassigned += 1
            continue
        day = stamp.date().isoformat()
        if not start <= day <= end:
            outside += 1
            continue
        model = record.get("model") or "unknown"
        require(isinstance(model, str) and MODEL.fullmatch(model), "invalid-usage-model")
        key = (day, project, client, model)
        row = buckets.setdefault(key, dict(zip(("date", "project", "client", "model"), key),
                                           **{f: 0 for f in COUNTS}))
        for field, source in FIELDS.items():
            row[field] = integer(row[field] + integer(record[source]))
        supplementary = record.get("supplementary", False)
        require(type(supplementary) is bool, "invalid-usage-supplement")
        row["records"] += 1
        row["supplementary_records"] += int(supplementary)
        row["manual_binding_records"] += int(manual is not None)
    require(len(buckets) <= MAX_BUCKETS, "usage-bucket-budget")
    return list(buckets.values()), {"export_records": len(raw["records"]), "unassigned_records": unassigned,
                                    "outside_window_records": outside}


def validate(value):
    require(set(value) == {"schema_version", "config_sha256", "projects", "buckets", "sources", "updated_at_unix_ms"}
            and value["schema_version"] == "shin.model-usage.v1", "invalid-usage-history")
    require(re.fullmatch(r"[a-f0-9]{64}", value["config_sha256"]) is not None, "invalid-usage-pin")
    integer(value["updated_at_unix_ms"])
    require(isinstance(value["projects"], list) and 1 <= len(value["projects"]) <= 24, "usage-project-budget")
    ids = set()
    for p in value["projects"]:
        require(set(p) == {"id", "name"} and SLUG.fullmatch(p["id"]) and p["id"] not in ids
                and isinstance(p["name"], str) and 1 <= len(p["name"]) <= 80
                and not any(ord(c) < 32 for c in p["name"]), "invalid-usage-project")
        ids.add(p["id"])
    require(isinstance(value["buckets"], list) and len(value["buckets"]) <= MAX_BUCKETS, "usage-bucket-budget")
    seen = set()
    for row in value["buckets"]:
        require(set(row) == {"date", "project", "client", "model", *COUNTS} and row["project"] in ids
                and row["client"] in CLIENTS and MODEL.fullmatch(row["model"]), "invalid-usage-bucket")
        require(date.fromisoformat(row["date"]).isoformat() == row["date"], "invalid-usage-date")
        key = tuple(row[f] for f in ("date", "project", "client", "model"))
        require(key not in seen, "duplicate-usage-bucket")
        seen.add(key)
        for field in COUNTS:
            integer(row[field])
        require(row["supplementary_records"] <= row["records"] and row["manual_binding_records"] <= row["records"],
                "invalid-usage-counts")
    require(isinstance(value["sources"], dict) and set(value["sources"]) <= set(CLIENTS), "invalid-usage-sources")
    for source in value["sources"].values():
        require(set(source) == {"state", "attempted_at_unix_ms", "last_success_unix_ms", "from", "to",
                               "export_records", "unassigned_records", "outside_window_records", "adapter_version"}
                and source["state"] in {"available", "unavailable"}
                and source["adapter_version"] == CODEBURN_VERSION, "invalid-usage-source")
        integer(source["attempted_at_unix_ms"])
        for field in ("last_success_unix_ms", "export_records", "unassigned_records", "outside_window_records"):
            if source[field] is not None:
                integer(source[field])
        for field in ("from", "to"):
            require(date.fromisoformat(source[field]).isoformat() == source[field], "invalid-usage-date")
        require(source["from"] <= source["to"], "invalid-usage-window")
    require(len(encode(value)) <= MAX_HISTORY, "usage-history-budget")
    return value


def report(home):
    return validate(read_private(Path(home) / "usage.json", MAX_HISTORY))


def save(home, value):
    data = encode(validate(value))
    with directory(home) as parent:
        fd = os.open("usage.pending", os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=parent)
        with os.fdopen(fd, "wb") as stream:
            private_file(stream.fileno())
            stream.truncate(0)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace("usage.pending", "usage.json", src_dir_fd=parent, dst_dir_fd=parent)
        os.fsync(parent)


def export(cfg, client, start, end, home):
    space = os.statvfs(home)
    require(space.f_bavail * space.f_frsize >= 1024**3, "usage-disk-reserve")
    env = {**os.environ, "CODEBURN_PRICING_SNAPSHOT_ONLY": "1", "NO_COLOR": "1"}
    # Full upstream exports can contain shell commands. Keep them private/ephemeral;
    # only the allowlisted numerical projection survives this context.
    with tempfile.TemporaryDirectory(prefix=".import-", dir=home) as scratch:
        target = Path(scratch) / "export.json"
        args = [cfg["codeburn"], "--timezone", "UTC", "export", "--provider", client,
                "--from", start, "--to", end, "--format", "json", "--output", str(target)]
        child = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True, umask=0o077)
        started = time.monotonic()
        try:
            while child.poll() is None:
                require(time.monotonic() - started < 120, "usage-export-timeout")
                require(not target.exists() or target.stat().st_size <= MAX_RAW, "usage-export-budget")
                time.sleep(.1)
            require(child.returncode == 0, "usage-export-failed")
            return read_private(target, MAX_RAW)
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
            child.wait()


def collect(cfg, home, *, today=None, loader=export):
    today = today or datetime.now(timezone.utc).date()
    end, start = today.isoformat(), (today - timedelta(days=29)).isoformat()
    cutoff = (today - timedelta(days=89)).isoformat()
    pin = hashlib.sha256(encode(cfg)).hexdigest()
    stamp = time.time_ns() // 1000000
    with directory(home) as parent:
        fd = os.open("writer.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600, dir_fd=parent)
        try:
            private_file(fd)
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                value = report(home)
            except FileNotFoundError:
                value = {"schema_version": "shin.model-usage.v1", "config_sha256": pin,
                         "projects": [{"id": p["id"], "name": p["name"]} for p in cfg["projects"]],
                         "buckets": [], "sources": {}, "updated_at_unix_ms": stamp}
            require(value["config_sha256"] == pin, "usage-config-changed-use-new-home")
            require(stamp >= value["updated_at_unix_ms"], "usage-clock-reversed")
            for client in CLIENTS:
                old = value["sources"].get(client, {})
                source = {"state": "unavailable", "attempted_at_unix_ms": stamp,
                          "last_success_unix_ms": old.get("last_success_unix_ms"),
                          "from": old.get("from", start), "to": old.get("to", end), "adapter_version": CODEBURN_VERSION,
                          **{f: old.get(f) for f in ("export_records", "unassigned_records", "outside_window_records")}}
                try:
                    rows, counts = normalize(loader(cfg, client, start, end, home), cfg, client, start, end)
                    require(not (old.get("export_records", 0) and not counts["export_records"]), "usage-empty-after-history")
                    # Replace one whole overlap window, not append cumulative snapshots.
                    value["buckets"] = [r for r in value["buckets"] if r["client"] != client or not start <= r["date"] <= end] + rows
                    source.update(state="available", last_success_unix_ms=stamp, **{"from": start, "to": end}, **counts)
                except Exception:
                    pass  # Preserve last good numerical history; never publish raw upstream errors.
                value["sources"][client] = source
            value["buckets"] = sorted((r for r in value["buckets"] if r["date"] >= cutoff),
                                      key=lambda r: tuple(r[f] for f in ("date", "project", "client", "model")))
            value["updated_at_unix_ms"] = stamp
            save(home, value)
            return value
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--dir", required=True, type=Path)
    parser.add_argument("--watch", action="store_true", help="collect every five minutes until interrupted")
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        cfg = config(args.config.expanduser().absolute())
        home = canonical(str(args.dir.expanduser().absolute()))
        home.mkdir(mode=0o700, exist_ok=True)
        with directory(home):
            pass
        require(all(not home.is_relative_to(Path(p)) for project in cfg["projects"] for p in project["paths"]),
                "usage-home-outside-projects-required")
        while True:
            version = subprocess.run([cfg["codeburn"], "--version"], capture_output=True, timeout=10, check=True).stdout.decode().strip()
            require(version == CODEBURN_VERSION, "unqualified-codeburn-version")
            value = collect(cfg, home)
            ok = all(s["state"] == "available" for s in value["sources"].values())
            print(encode({"ok": ok, "buckets": len(value["buckets"]),
                          "sources": {k: v["state"] for k, v in value["sources"].items()}}).decode(), flush=True)
            if not args.watch:
                return 0 if ok else 1
            time.sleep(300)
    except KeyboardInterrupt:
        return 0
    except Exception:
        print('{"ok":false,"error":{"code":"model-usage-rejected"}}', file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
