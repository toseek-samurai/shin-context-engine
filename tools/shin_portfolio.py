#!/usr/bin/env python3
"""Serial, read-only consumer assessment with external paired Core indexes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import stat
import subprocess
import sys
import time
import uuid

sys.dont_write_bytecode = True
from shin import load_semantic

load_semantic()
from shin_semantic.artifact import canonical, decode, encode, require
from shin_semantic.telemetry import directory
from shin_eval_review import review, review_query

MAX_REPORT = 2 * 1024 * 1024
MAX_RAW = 32 * 1024 * 1024
MAX_PROJECTS = 24
SLUG = re.compile(r"[a-z0-9][a-z0-9-]{0,79}\Z")
STATUSES = {"pending", "running", "compared", "source-changed", "blocked", "interrupted"}
KINDS = {"passed", "existing-failure", "regression", "improvement", "unknown"}
STAGES = {"live-before", "live-after", "baseline-build", "candidate-build", "baseline-validate",
          "candidate-validate", "baseline-current", "candidate-current"}


def now():
    return time.time_ns() // 1000000


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path, maximum=MAX_RAW):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= maximum, "portfolio-input-budget")
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, "portfolio-input-budget")
    return decode(data)


def private_file(fd):
    info = os.fstat(fd)
    require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_nlink == 1
            and stat.S_IMODE(info.st_mode) == 0o600, "unsafe-portfolio-file")


def save_report(home, value):
    data = encode(value)
    require(len(data) <= MAX_REPORT, "portfolio-report-budget")
    with directory(home) as parent:
        fd = os.open("report.pending", os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK,
                     0o600, dir_fd=parent)
        with os.fdopen(fd, "wb") as stream:
            private_file(stream.fileno())
            stream.truncate(0)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace("report.pending", "report.json", src_dir_fd=parent, dst_dir_fd=parent)
        os.fsync(parent)


def _number(value):
    require(type(value) is int and 0 <= value <= 2**63 - 1, "invalid-portfolio-number")
    return value


def _text(value):
    require(isinstance(value, str) and len(value) <= 160 and not any(ord(c) < 32 for c in value),
            "invalid-portfolio-text")
    return value


def _count(value):
    require(isinstance(value, dict) and set(value) == {"passed", "failed", "total"}, "invalid-portfolio-count")
    result = {k: _number(v) for k, v in value.items()}
    require(result["passed"] + result["failed"] == result["total"], "invalid-portfolio-count")
    return result


def _pin(value):
    require(value is None or isinstance(value, str) and re.fullmatch(r"[a-f0-9]{64}", value), "invalid-portfolio-pin")
    return value


def panel_projection(value):
    """Allowlist report metadata; never serve raw CLI results, queries or paths."""
    require(value.get("schema_version") == "shin.portfolio.v1", "invalid-portfolio-schema")
    require(value["status"] in {"running", "completed", "interrupted"}, "invalid-portfolio-status")
    require(re.fullmatch(r"[a-f0-9]{32}", value["run_id"]), "invalid-portfolio-id")
    projects = value["projects"]
    require(isinstance(projects, list) and len(projects) <= MAX_PROJECTS, "portfolio-project-budget")
    result = {"schema_version": "shin.portfolio.v1", "run_id": value["run_id"], "status": value["status"],
              "started_at_unix_ms": _number(value["started_at_unix_ms"]),
              "updated_at_unix_ms": _number(value["updated_at_unix_ms"]),
              "scope": "native-core-declared-checks-shadow-only", "agent_usefulness": None,
              "semantic_quality": None, "live_traffic_coverage": None,
              "projects": [], "engines": {}, "excluded": []}
    result["runner_sha256"] = _pin(value.get("runner_sha256"))
    for name in ("baseline", "candidate"):
        engine = value["engines"][name]
        require(re.fullmatch(r"[a-f0-9]{64}", engine["sha256"]), "invalid-portfolio-pin")
        result["engines"][name] = {k: _text(engine[k]) for k in ("version", "generator_version", "sha256")}
    for row in projects:
        require(SLUG.fullmatch(row["id"]) and SLUG.fullmatch(row["profile"]), "invalid-portfolio-project")
        require(row["status"] in STATUSES, "invalid-portfolio-project-status")
        item = {k: _text(row[k]) for k in ("id", "profile", "status", "stage", "reason")}
        item["captured_at_unix_ms"] = _number(row["captured_at_unix_ms"])
        require(type(row["comparable"]) is bool, "invalid-portfolio-comparison")
        item["comparable"] = row["comparable"]
        item["profile_sha256"] = _pin(row.get("profile_sha256"))
        item["live"] = {}
        for name, observed in row["live"].items():
            require(name in {"before", "after"} and observed["state"] in {"ready", "not-ready", "unavailable"},
                    "invalid-portfolio-live-state")
            item["live"][name] = {"state": observed["state"], "captured_at_unix_ms": _number(observed["captured_at_unix_ms"])}
            action = observed.get("next_action", "unknown")
            require(SLUG.fullmatch(action), "invalid-portfolio-action")
            item["live"][name]["next_action"] = action
            for field in ("worktree_current", "validation_passed"):
                require(observed.get(field) is None or type(observed[field]) is bool, "invalid-portfolio-live-state")
                item["live"][name][field] = observed.get(field)
        item["lanes"] = {}
        for name, lane in row["lanes"].items():
            require(name in {"baseline", "candidate"}, "invalid-portfolio-lane")
            copied = {"checks": _count(lane["checks"]), "pack": _count(lane["pack"]),
                      "query": _count(lane["query"]), "source_files": _number(lane["source_files"]),
                      "quality_failed": _number(lane["quality_failed"]),
                      "resource_exceeded": _number(lane["resource_exceeded"]),
                      "unknown": _number(lane["unknown"]), "wall_ms": _number(lane["wall_ms"]),
                      "resource_policy": _text(lane["resource_policy"])}
            copied["skipped_large_files"] = _number(lane.get("skipped_large_files", 0))
            copied["input_inventory_sha256"] = _pin(lane.get("input_inventory_sha256"))
            require(type(lane["current"]) is bool, "invalid-portfolio-currentness")
            copied["current"] = lane["current"]
            item["lanes"][name] = copied
        item["cases"] = []
        require(len(row["cases"]) <= 512, "portfolio-case-budget")
        for case in row["cases"]:
            require(case["kind"] in KINDS and case["suite"] in {"eval", "pack"}, "invalid-portfolio-case")
            copied = {k: _text(case[k]) for k in ("id", "suite", "kind", "baseline", "candidate")}
            require(type(case["variant"]) is int and -1 <= case["variant"] <= 7, "invalid-portfolio-variant")
            copied["variant"] = case["variant"]
            item["cases"].append(copied)
        item["gates"] = []
        for gate in row["gates"]:
            require(gate["kind"] in KINDS, "invalid-portfolio-gate")
            item["gates"].append({k: _text(gate[k]) for k in ("id", "kind", "baseline", "candidate")})
        result["projects"].append(item)
    require(len({p["id"] for p in result["projects"]}) == len(projects), "duplicate-portfolio-project")
    require(len(value.get("excluded", [])) <= MAX_PROJECTS, "portfolio-exclusion-budget")
    result["excluded"] = [_text(entry) for entry in value.get("excluded", [])]
    events = value.get("events", [])
    require(isinstance(events, list) and len(events) <= MAX_PROJECTS * len(STAGES), "portfolio-event-budget")
    result["events"] = []
    identities = set()
    for event in events:
        identity = (event["project"], event["stage"])
        require(event["project"] in {p["id"] for p in projects} and event["stage"] in STAGES
                and identity not in identities, "invalid-portfolio-event")
        identities.add(identity)
        require(event["state"] in {"running", "completed", "failed", "interrupted"}, "invalid-event-state")
        running = event["state"] == "running"
        require((event["ended_at_unix_ms"] is None) == running
                and (event["elapsed_ms"] is None) == running, "invalid-event-time")
        require(event["outcome"] in {None, "passed", "issues", "unavailable"}, "invalid-event-outcome")
        require((running and event["outcome"] is None)
                or (event["state"] == "completed" and event["outcome"] in {"passed", "issues"})
                or (event["state"] in {"failed", "interrupted"} and event["outcome"] == "unavailable"),
                "inconsistent-event-outcome")
        result["events"].append({"project": event["project"], "stage": event["stage"], "state": event["state"],
            "started_at_unix_ms": _number(event["started_at_unix_ms"]),
            "ended_at_unix_ms": None if running else _number(event["ended_at_unix_ms"]),
            "elapsed_ms": None if running else _number(event["elapsed_ms"]), "outcome": event["outcome"]})
    return result


def report(home):
    with directory(home) as parent:
        fd = os.open("report.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        with os.fdopen(fd, "rb") as stream:
            private_file(stream.fileno())
            require(os.fstat(stream.fileno()).st_size <= MAX_REPORT, "portfolio-report-budget")
            data = stream.read(MAX_REPORT + 1)
    require(len(data) <= MAX_REPORT, "portfolio-report-budget")
    return panel_projection(decode(data))


def inventory(manifest):
    result = {}
    for key in ("sources", "source_fact_sources"):
        require(isinstance(manifest[key], list), "inventory-unavailable")
        for item in manifest[key]:
            path, content = item["path"], item.get("content_hash")
            require(isinstance(path, str) and not Path(path).is_absolute() and ".." not in Path(path).parts,
                    "invalid-inventory-path")
            if key == "source_fact_sources" and item.get("status") == "skipped-large" and content == "":
                # Excluded bodies are not evidence. Compare the exclusion, not nonexistent content hashes.
                require(path not in result, "inconsistent-source-inventory")
                result[path] = {"excluded": "skipped-large", "bytes": _number(item["bytes"])}
                continue
            require(isinstance(content, str) and re.fullmatch(r"fnv1a64:[a-f0-9]{16}", content),
                    "inventory-hash-unavailable")
            require(path not in result or result[path] == content, "inconsistent-source-inventory")
            result[path] = content
    require(result, "empty-source-inventory")
    return result


def classify(before, after):
    if before is None or after is None:
        return "unknown"
    return { (True, True): "passed", (False, False): "existing-failure",
             (True, False): "regression", (False, True): "improvement" }[(before, after)]


def summarize_validation(value):
    require(value["schema_version"] == "shin.validation.v1", "full-validation-required")
    gates = {g["id"]: g for g in value["gates"] if g["attempted"]}
    require(set(gates) == {"profile-check", "check", "doctor", "vacuum", "eval", "pack-eval"},
            "unexpected-validation-gates")
    evaluation, pack = gates["eval"]["result"], gates["pack-eval"]["result"]
    review(evaluation, hashlib.sha256(encode(evaluation)).hexdigest())
    policy = evaluation.get("resource_check", {}).get("policy", "strict")
    rows = []
    for case in evaluation["cases"]:
        rows.append(review_query(case, True, policy))
        rows.extend(review_query(variant, True, policy) for variant in case.get("query_variants", []))
    require(pack["schema_version"] == "shin.pack-eval.v1" and len(pack["cases"]) <= 256, "invalid-pack-eval")
    pack_count = _count(pack["summary"])
    require(len(pack["cases"]) == pack_count["total"] and
            sum(c["passed"] is True for c in pack["cases"]) == pack_count["passed"], "invalid-pack-count")
    checks = {"total": len(gates), "passed": sum(g["ok"] is True for g in gates.values()),
              "failed": sum(g["ok"] is not True for g in gates.values())}
    cases = {("eval", row["id"], row["variant_index"]):
             (None if row["quality_status"] == "unknown" else row["quality_status"] != "failed", row["classification"])
             for row in rows}
    for row in pack["cases"]:
        require(type(row["passed"]) is bool and ("pack", row["id"], -1) not in cases, "invalid-pack-case")
        cases[("pack", row["id"], -1)] = (row["passed"], "passed" if row["passed"] else "failed")
    summary = {"checks": checks, "query": _count(evaluation["query_summary"]), "pack": pack_count,
               "quality_failed": sum(row["quality_status"] == "failed" for row in rows),
               "unknown": sum(row["quality_status"] == "unknown" for row in rows),
               "resource_exceeded": sum(any(m["exceeded"] for m in row["resource_limits"]) for row in rows),
               "resource_policy": policy}
    return summary, cases, {k: g["ok"] for k, g in gates.items()}


def compare(before, after):
    return [{"suite": key[0], "id": key[1], "variant": key[2],
             "kind": classify(before.get(key, (None, "missing"))[0], after.get(key, (None, "missing"))[0]),
             "baseline": before.get(key, (None, "missing"))[1],
             "candidate": after.get(key, (None, "missing"))[1]}
            for key in sorted(before.keys() | after.keys())]


def execute(command, prefix, timeout, pulse):
    """Bound output on disk as well as time; never buffer private CLI output in logs."""
    started = time.monotonic_ns()
    with prefix.with_suffix(".json").open("xb") as out, prefix.with_suffix(".stderr").open("xb") as err:
        os.fchmod(out.fileno(), 0o600)
        os.fchmod(err.fileno(), 0o600)
        process = subprocess.Popen(command, stdout=out, stderr=err, stdin=subprocess.DEVNULL)
        try:
            last_pulse = 0
            while process.poll() is None:
                elapsed = (time.monotonic_ns() - started) / 1e9
                require(elapsed <= timeout, "command-timeout")
                require(shutil.disk_usage(prefix.parent).free >= 1024 ** 3, "free-disk-reserve")
                require(os.fstat(out.fileno()).st_size + os.fstat(err.fileno()).st_size <= MAX_RAW,
                        "command-output-budget")
                if elapsed - last_pulse >= 5:
                    pulse()
                    last_pulse = elapsed
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        require(os.fstat(out.fileno()).st_size + os.fstat(err.fileno()).st_size <= MAX_RAW,
                "command-output-budget")
    value = read_json(prefix.with_suffix(".json"))
    return value, process.returncode, (time.monotonic_ns() - started) // 1000000


def prepare(config, home):
    require(config.get("schema_version") == "shin.portfolio-config.v1", "invalid-portfolio-config")
    require(set(config) <= {"schema_version", "baseline", "candidate", "projects", "excluded", "timeout_seconds"},
            "unknown-portfolio-option")
    timeout = config.get("timeout_seconds", 180)
    require(type(timeout) is int and 5 <= timeout <= 600, "invalid-command-timeout")
    require(isinstance(config["projects"], list) and 1 <= len(config["projects"]) <= MAX_PROJECTS,
            "portfolio-project-budget")
    protected = []
    engines = {}
    for lane in ("baseline", "candidate"):
        binary = canonical(config[lane])
        require(binary.is_file() and os.access(binary, os.X_OK), "engine-unavailable")
        protected.append(binary)
        engines[lane] = binary
    projects = []
    for entry in config["projects"]:
        require(set(entry) == {"id", "profile", "root", "registry", "canonical_index", "marker", "live_engine"},
                "invalid-project-route")
        require(SLUG.fullmatch(entry["id"]) and SLUG.fullmatch(entry["profile"]), "invalid-project-identity")
        project = dict(entry)
        for name in ("root", "registry", "canonical_index"):
            project[name] = canonical(entry[name])
            require(project[name].is_dir(), "route-directory-unavailable")
            protected.append(project[name])
        project["profile_file"] = project["registry"] / (entry["profile"] + ".json")
        require(project["profile_file"].is_file(), "profile-unavailable")
        require(entry["live_engine"] in engines, "invalid-live-engine")
        if entry["marker"] is not None:
            marker = Path(entry["marker"])
            require(not marker.is_absolute() and ".." not in marker.parts, "invalid-marker")
            require((project["root"] / marker).is_file(), "marker-unavailable")
        projects.append(project)
    require(len({p["id"] for p in projects}) == len(projects), "duplicate-project")
    for path in protected:
        require(not home.is_relative_to(path) and not path.is_relative_to(home), "portfolio-route-overlap")
    require(not home.exists(), "new-portfolio-home-required")
    return engines, projects, timeout


def run(config, home):
    home = canonical(str(home))
    engines, projects, timeout = prepare(config, home)
    home.mkdir(mode=0o700)
    value = {"schema_version": "shin.portfolio.v1", "run_id": uuid.uuid4().hex, "status": "running",
             "started_at_unix_ms": now(), "updated_at_unix_ms": now(), "engines": {}, "projects": [],
             "excluded": config.get("excluded", []), "events": []}
    value["runner_sha256"] = digest(Path(__file__).resolve())

    def pulse():
        value["updated_at_unix_ms"] = now()
        save_report(home, value)

    for name, binary in engines.items():
        pin = digest(binary)
        version, code, _ = execute([str(binary), "--version"], home / name, timeout, lambda: None)
        require(code == 0 and digest(binary) == pin, "engine-identity-changed")
        value["engines"][name] = {"sha256": pin, "version": version["version"],
                                  "generator_version": version["generator_version"]}
    for project in projects:
        value["projects"].append({"id": project["id"], "profile": project["profile"], "status": "pending",
                                  "stage": "queued", "reason": "", "captured_at_unix_ms": now(),
                                  "comparable": False, "live": {}, "lanes": {}, "cases": [], "gates": []})
    pulse()
    try:
        for project, row in zip(projects, value["projects"]):
            row["status"] = "running"
            local = home / project["id"]
            local.mkdir(mode=0o700)

            def command(lane, out, args, label):
                row["stage"] = label
                event = {"project": project["id"], "stage": label, "state": "running",
                         "started_at_unix_ms": now(), "ended_at_unix_ms": None, "elapsed_ms": None, "outcome": None}
                require(label in STAGES and len(value["events"]) < MAX_PROJECTS * len(STAGES), "portfolio-event-budget")
                value["events"].append(event)
                pulse()
                argv = [str(engines[lane]), "--profile", project["profile"], "--profile-dir", str(project["registry"]),
                        "--root", str(project["root"]), "--out-dir", str(out), *args]
                started = time.monotonic_ns()
                try:
                    result = execute(argv, local / label, timeout, pulse)
                    event.update(state="completed", outcome="passed" if result[1] == 0 else "issues")
                    return result
                except BaseException as error:
                    event.update(state="interrupted" if isinstance(error, KeyboardInterrupt) else "failed", outcome="unavailable")
                    raise
                finally:
                    event.update(ended_at_unix_ms=now(), elapsed_ms=(time.monotonic_ns() - started) // 1000000)
                    pulse()

            def live(when):
                try:
                    args = ["ready", "--strict"]
                    if project["marker"]:
                        args += ["--marker", project["marker"]]
                    result, code, _ = command(project["live_engine"], project["canonical_index"], args, "live-" + when)
                    require(result.get("command") == "ready" and type(result.get("ready")) is bool,
                            "invalid-readiness")
                    state = "ready" if code == 0 and result["ready"] else "not-ready"
                    observed = {"next_action": result.get("next_action", "unknown"),
                                "worktree_current": result.get("summary", {}).get("worktree_current"),
                                "validation_passed": result.get("summary", {}).get("validation_passed")}
                except Exception:
                    state = "unavailable"
                    observed = {}
                row["live"][when] = {"state": state, "captured_at_unix_ms": now(), **observed}

            try:
                profile_pin = digest(project["profile_file"])
                row["profile_sha256"] = profile_pin
                live("before")
                inputs, cases, gates = {}, {}, {}
                for lane in ("baseline", "candidate"):
                    out = local / (lane + "-index")
                    built, code, build_ms = command(lane, out, ["build"], lane + "-build")
                    require(code == 0 and built.get("ok") is True, "shadow-build-failed")
                    manifest = read_json(out / "docs/kb/generated/manifest.json")
                    inputs[lane] = inventory(manifest)
                    checked, code, validate_ms = command(lane, out, ["validate", "--strict", "--require-current"],
                                                         lane + "-validate")
                    require(code in (0, 1) and checked.get("profile") == project["profile"], "validation-incomplete")
                    summary, cases[lane], gates[lane] = summarize_validation(checked)
                    skipped = sum(isinstance(v, dict) for v in inputs[lane].values())
                    summary.update(source_files=len(inputs[lane]) - skipped, skipped_large_files=skipped,
                                   input_inventory_sha256=hashlib.sha256(encode(inputs[lane])).hexdigest(),
                                   wall_ms=build_ms + validate_ms, current=False)
                    row["lanes"][lane] = summary
                    pulse()
                live("after")
                for lane in ("baseline", "candidate"):
                    diff, code, _ = command(lane, local / (lane + "-index"), ["worktree-diff", "--strict"], lane + "-current")
                    row["lanes"][lane]["current"] = (code == 0 and diff.get("current") is True
                                                      and diff.get("comparison", {}).get("snapshot_integrity") == "verified")
                require(all(digest(engines[lane]) == value["engines"][lane]["sha256"] for lane in engines),
                        "engine-identity-changed")
                if digest(project["profile_file"]) != profile_pin or not all(lane["current"] for lane in row["lanes"].values()):
                    row.update(status="source-changed", reason="source-or-profile-changed-during-run")
                elif inputs["baseline"] != inputs["candidate"]:
                    row.update(status="blocked", reason="source-inventories-differ")
                else:
                    row.update(status="compared", comparable=True, cases=compare(cases["baseline"], cases["candidate"]))
                    row["gates"] = [{"id": name, "kind": classify(gates["baseline"][name], gates["candidate"][name]),
                                     "baseline": "passed" if gates["baseline"][name] else "failed",
                                     "candidate": "passed" if gates["candidate"][name] else "failed"}
                                    for name in sorted(gates["baseline"])]
            except Exception as error:
                # Store only our enumerated failure code, never private exception text.
                reason = str(error)
                allowed = {"shadow-build-failed", "inventory-hash-unavailable", "inconsistent-source-inventory",
                           "empty-source-inventory", "engine-identity-changed", "validation-incomplete",
                           "command-timeout", "command-output-budget"}
                allowed.add("free-disk-reserve")
                row.update(status="blocked", reason=reason if reason in allowed else "assessment-incomplete")
            row.update(stage="finished", captured_at_unix_ms=now())
            pulse()
            print(json.dumps({"project": row["id"], "status": row["status"], "reason": row["reason"]}), flush=True)
        value["status"] = "completed"
    except BaseException:
        value["status"] = "interrupted"
        for row in value["projects"]:
            if row["status"] in {"pending", "running"}:
                row.update(status="interrupted", reason="runner-interrupted")
        raise
    finally:
        pulse()
    panel_projection(value)
    return value


def main():
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="new external private directory, never a consumer index")
    args = parser.parse_args()
    try:
        result = run(read_json(args.config, MAX_REPORT), args.out.expanduser().absolute())
    except KeyboardInterrupt:
        return 130
    except Exception:
        print('{"ok":false,"error":"portfolio-run-rejected"}', file=sys.stderr)
        return 1
    comparable = [row for row in result["projects"] if row["comparable"]]
    clean = len(comparable) == len(result["projects"]) and all(
        case["kind"] not in {"regression", "unknown"} for row in comparable for case in row["cases"] + row["gates"])
    return 0 if clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
