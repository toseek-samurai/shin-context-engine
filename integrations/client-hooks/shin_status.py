#!/usr/bin/env python3
"""Opt-in Codex SessionStart adapter for cached, read-only Observer status."""

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import stat
import subprocess
import sys
import time

MAX_INPUT = 16 * 1024
MAX_CONFIG = MAX_CACHE = 64 * 1024
MAX_OBSERVER_OUTPUT = 512 * 1024
MAX_OUTPUT = 1024
MAX_ENTRIES = 128
CHILD_TIMEOUT = 1.0
RECURSION_FLAG = "SHIN_STATUS_HOOK_ACTIVE"
SHA = re.compile(r"[0-9a-f]{64}")
SLUG = re.compile(r"[a-z][a-z0-9-]{0,62}")
IDENTITY = re.compile(r"[0-9]{1,32}:[0-9]{1,32}")
FINGERPRINT = re.compile(r"fnv1a64:[0-9a-f]{16}")


def require(condition):
    if not condition:
        raise ValueError("Invalid hook input or binding")


def decode(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result)
            result[key] = value
        return result

    def nonfinite(_):
        raise ValueError("Nonfinite JSON")

    return json.loads(data, object_pairs_hook=pairs, parse_constant=nonfinite)


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    require(isinstance(value, str) and 0 < len(value.encode()) <= 4096 and "\0" not in value)
    path = Path(value)
    require(path.is_absolute() and str(path) == value and path != Path("/"))
    try:
        require(path.resolve() == path)
    except RuntimeError:
        raise ValueError("Cyclic hook path") from None
    return path


def identity(path):
    info = path.stat()
    require(stat.S_ISDIR(info.st_mode))
    return f"{info.st_dev}:{info.st_ino}"


def overlap(left, right):
    return left.is_relative_to(right) or right.is_relative_to(left)


def check_file(info, private):
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1)
    require(info.st_uid == os.getuid() if private else info.st_uid in (os.getuid(), 0))
    require(stat.S_IMODE(info.st_mode) == 0o600 if private else not info.st_mode & 0o022)


def read_file(path, limit, private=True):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        check_file(os.fstat(stream.fileno()), private)
        require(os.fstat(stream.fileno()).st_size <= limit)
        data = stream.read(limit + 1)
        require(len(data) <= limit)
        return data


def read_pipe(fd, limit, deadline):
    data = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError("Hook deadline")
        block = os.read(fd, min(65536, limit + 1 - len(data)))
        if not block:
            return bytes(data)
        data.extend(block)
        require(len(data) <= limit)


def observer_status(binary, config):
    process = subprocess.Popen([str(binary), "status", "--config", str(config)],
        cwd="/", stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        start_new_session=True, env={"PATH": "", "LC_ALL": "C", RECURSION_FLAG: "1"})
    deadline = time.monotonic() + CHILD_TIMEOUT
    try:
        data = read_pipe(process.stdout.fileno(), MAX_OBSERVER_OUTPUT, deadline)
        require(process.wait(timeout=max(0.001, deadline - time.monotonic())) == 0)
        return decode(data)
    finally:
        # An unreaped leader also protects its process-group ID from reuse.
        if process.returncode is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        process.stdout.close()


def load_registry(path):
    path = canonical(str(path))
    registry = decode(read_file(path, MAX_CONFIG))
    require(isinstance(registry, dict) and set(registry) == {
        "schema_version", "observer_binary", "observer_sha256", "cache_dir", "bindings"})
    require(registry["schema_version"] == "shin.client-status-hook.v1")
    require(isinstance(registry["observer_sha256"], str) and SHA.fullmatch(registry["observer_sha256"]))
    canonical(registry["observer_binary"])
    cache = canonical(registry["cache_dir"])
    require(cache.stat().st_uid == os.getuid() and stat.S_IMODE(cache.stat().st_mode) == 0o700 and cache.is_dir())
    require(not path.is_relative_to(cache) and not Path(registry["observer_binary"]).is_relative_to(cache))
    bindings = registry["bindings"]
    require(isinstance(bindings, list) and 0 < len(bindings) <= 32)
    for binding in bindings:
        require(isinstance(binding, dict) and set(binding) == {
            "root", "root_identity", "observer_config", "observer_config_sha256", "binding_key"})
        root, config = canonical(binding["root"]), canonical(binding["observer_config"])
        require(not overlap(root, cache) and not path.is_relative_to(root) and not config.is_relative_to(root))
        require(not config.is_relative_to(cache))
        for key in ("observer_config_sha256", "binding_key"):
            require(isinstance(binding[key], str) and SHA.fullmatch(binding[key]))
        require(isinstance(binding["root_identity"], str) and IDENTITY.fullmatch(binding["root_identity"]))
    return registry


def binding_config(registry, binding):
    config_path = Path(binding["observer_config"])
    data = read_file(config_path, MAX_CONFIG)
    require(digest(data) == binding["observer_config_sha256"])
    config = decode(data)
    require(isinstance(config, dict) and config["schema_version"] == "shin.observer-config.v2"
            and config["root"] == binding["root"])
    cache = Path(registry["cache_dir"])
    for key in ("state_dir", "out_dir", "profile_dir"):
        require(not overlap(cache, canonical(config[key])))
    for key in ("project_id", "workspace_id", "profile"):
        require(isinstance(config[key], str) and SLUG.fullmatch(config[key]))
    return config


def project_status(registry, binding, config):
    require(identity(Path(binding["root"])) == binding["root_identity"])
    config_path = Path(binding["observer_config"])
    binary = Path(registry["observer_binary"])
    require(digest(read_file(binary, 64 * 1024 * 1024, private=False)) == registry["observer_sha256"])
    payload = observer_status(binary, config_path)
    require(digest(read_file(config_path, MAX_CONFIG)) == binding["observer_config_sha256"])
    require(identity(Path(binding["root"])) == binding["root_identity"])
    require(isinstance(payload, dict) and payload.get("schema_version") == "shin.observer-response.v1" and
            payload.get("tool") == "shin-observer" and payload.get("ok") is True)
    result = payload["result"]
    require(isinstance(result, dict))
    for key in ("live_source_verified", "observer_state_mutated", "consumer_mutated", "index_mutated"):
        require(result[key] is False)
    age = result["observation_age"]
    require(age in ("within-ttl", "expired", "time-uncertain", "unavailable"))
    require(type(result["writer_lock_held"]) is bool)
    projected = {"status": "unobserved", "age": age, "writer": result["writer_lock_held"],
                 "route": "/".join(config[key] for key in ("project_id", "workspace_id", "profile"))}
    state = result["state"]
    if state is None:
        return projected
    require(isinstance(state, dict) and state["schema_version"] == "shin.observer-state.v3"
            and state["binding_key"] == binding["binding_key"]
            and state["root_identity"] == binding["root_identity"])
    for key in ("root", "profile", "profile_dir", "out_dir", "state_dir", "project_id", "workspace_id"):
        require(state["binding"][key] == config[key])
    observation = state["observation"]
    if observation is None:
        return projected
    require(isinstance(observation, dict) and
            observation["status"] in ("current", "stale", "unpublished", "incomplete", "error"))
    for key in ("readiness_assessed", "source_mutated", "index_mutated"):
        require(observation[key] is False)
    if observation["status"] == "current":
        require(observation["snapshot_current"] is True)
    projected["status"] = observation["status"]
    for key in ("snapshot_id", "worktree_id", "profile_config_hash"):
        value = observation[key]
        require(value is None or (isinstance(value, str) and FINGERPRINT.fullmatch(value)))
        projected[key] = value
    return projected


@contextmanager
def locked_cache(directory):
    fd = os.open(directory / "status.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, 0o600)
    try:
        check_file(os.fstat(fd), True)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def update_cache(directory, key, fingerprint, force):
    path = directory / "status.json"
    try:
        cache = decode(read_file(path, MAX_CACHE))
    except FileNotFoundError:
        cache = {"schema_version": "shin.client-status-cache.v1", "entries": {}}
    require(isinstance(cache, dict) and set(cache) == {"schema_version", "entries"}
            and cache["schema_version"] == "shin.client-status-cache.v1")
    entries = cache["entries"]
    require(isinstance(entries, dict) and len(entries) <= MAX_ENTRIES)
    now = int(time.time())
    for item, value in entries.items():
        require(SHA.fullmatch(item) and isinstance(value, dict) and set(value) == {"fingerprint", "seen"})
        require(isinstance(value["fingerprint"], str) and SHA.fullmatch(value["fingerprint"]))
        require(type(value["seen"]) is int and 0 <= value["seen"] <= now)
    entries = {k: v for k, v in entries.items() if now - v["seen"] <= 86400}
    if not force and entries.get(key, {}).get("fingerprint") == fingerprint:
        return False
    entries.pop(key, None)
    entries = dict(sorted(entries.items(), key=lambda row: (row[1]["seen"], row[0]), reverse=True)[:MAX_ENTRIES - 1])
    entries[key] = {"fingerprint": fingerprint, "seen": now}
    data = encoded({"schema_version": "shin.client-status-cache.v1", "entries": entries})
    require(len(data) <= MAX_CACHE)
    temporary = directory / ("status-" + os.urandom(8).hex() + ".tmp")
    try:
        fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    return True


def handle(event, registry_path):
    if os.environ.get(RECURSION_FLAG) == "1" or not isinstance(event, dict):
        return {}
    if event.get("hook_event_name") != "SessionStart" or event.get("source") not in ("startup", "resume", "clear", "compact"):
        return {}
    session = event.get("session_id")
    require(isinstance(session, str) and 0 < len(session.encode()) <= 256 and "\0" not in session)
    cwd = canonical(event.get("cwd"))
    registry = load_registry(registry_path)
    matches = [b for b in registry["bindings"] if cwd.is_relative_to(Path(b["root"]))]
    if len(matches) != 1:
        return {}
    binding = matches[0]
    # Check every enrolled protected directory before writing even the cache lock.
    configs = [binding_config(registry, candidate) for candidate in registry["bindings"]]
    config = configs[registry["bindings"].index(binding)]
    key = digest(encoded({"session": session, "binding": binding}))
    with locked_cache(Path(registry["cache_dir"])):
        try:
            status = project_status(registry, binding, config)
        except (OSError, ValueError, KeyError, TypeError, RecursionError, subprocess.SubprocessError):
            status = {"status": "unavailable", "age": "unavailable", "writer": False, "route": "registered-binding"}
        message = (f"Shin cached observation: {status['status']} ({status['age']}); "
                   f"observer: {'active' if status['writer'] else 'idle-or-unavailable'}; route: {status['route']}. "
                   "This is not readiness or live-source verification. Use the enrolled Shin ready --strict route before retrieval. "
                   "No index build or network delivery was requested.")
        output = {"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": message}}
        require(len(encoded(output)) <= MAX_OUTPUT)
        if not update_cache(Path(registry["cache_dir"]), key, digest(encoded(status)), event["source"] in ("compact", "clear")):
            return {}
        return output


def main():
    output = {}
    try:
        require(len(sys.argv) == 3 and sys.argv[1] == "--config")
        data = read_pipe(sys.stdin.fileno(), MAX_INPUT, time.monotonic() + 0.25)
        output = handle(decode(data), Path(sys.argv[2]))
    except (OSError, ValueError, KeyError, TypeError, RecursionError, subprocess.SubprocessError):
        pass
    sys.stdout.buffer.write(encoded(output) + b"\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
