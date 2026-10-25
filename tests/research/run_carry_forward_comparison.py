#!/usr/bin/env python3
"""Freeze explicit corpora and compare CLI lanes without changing their owners."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def freeze(spec, output):
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    inventories = {}
    cohorts = []
    for case in spec:
        original = Path(case["root"]).resolve()
        if str(original) not in inventories:
            target = output / "sources" / case["id"]
            listed = subprocess.check_output(
                ["git", "-C", str(original), "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
            ).decode().split("\0")
            rows = []
            skipped_links = []
            for name in sorted(set(filter(None, listed))):
                source = original / name
                if source.is_symlink():
                    skipped_links.append({"path": name, "target": str(source.readlink())})
                    continue
                if not source.exists():
                    continue
                if not source.is_file():
                    raise RuntimeError(f"Review non-regular input explicitly: {source}")
                destination = target / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                before = digest(source)
                shutil.copy2(source, destination)
                if digest(destination) != before or digest(source) != before:
                    raise RuntimeError(f"Source changed during freeze: {source}")
                rows.append({"path": name, "sha256": before, "bytes": destination.stat().st_size})
            # Detect a changed earlier input after the complete copy, not only per-file drift.
            for row in rows:
                if digest(original / row["path"]) != row["sha256"]:
                    raise RuntimeError(f"Source changed during freeze: {original / row['path']}")
            inventories[str(original)] = {
                "root": str(target), "original": str(original), "files": rows,
                "excluded_symlinks": skipped_links,
                "head": subprocess.check_output(["git", "-C", str(original), "rev-parse", "HEAD"], text=True).strip(),
            }
        inventory = inventories[str(original)]
        registry = output / "profiles" / case["id"]
        registry.mkdir(parents=True)
        source_profile = Path(case["profile"])
        profile_hash = digest(source_profile)
        shutil.copy2(source_profile, registry / source_profile.name)
        if digest(source_profile) != profile_hash or digest(registry / source_profile.name) != profile_hash:
            raise RuntimeError("Profile changed during freeze")
        config = json.loads((registry / source_profile.name).read_text())
        cohorts.append({"id": case["id"], "root": inventory["root"],
                        "profile_id": config["id"], "profile_dir": str(registry),
                        "profile_sha256": profile_hash})
    save(output / "frozen.json", {"cohorts": cohorts, "inventories": inventories,
                                  "boundary": "Working bytes including tracked local changes; symlinks excluded and recorded, never followed. No consumer writes. Not a Git commit or external index identity."})


def invoke(binary, arguments, destination):
    start = time.monotonic()
    result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=300)
    elapsed = (time.monotonic() - start) * 1000
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.with_suffix(".stdout").write_text(result.stdout)
    destination.with_suffix(".stderr").write_text(result.stderr)
    value = json.loads(result.stdout or result.stderr)
    save(destination, {"exit_code": result.returncode, "wall_ms": elapsed, "result": value})
    return value


def run(output, label, binary):
    frozen = json.loads((output / "frozen.json").read_text())
    for inventory in frozen["inventories"].values():
        for row in inventory["files"]:
            if digest(Path(inventory["root"]) / row["path"]) != row["sha256"]:
                raise RuntimeError("Frozen source drift")
    lane = output / "lanes" / label
    lane.mkdir(parents=True, exist_ok=False)
    binary = binary.resolve(strict=True)
    identity = {"path": str(binary), "sha256": digest(binary),
                "version": json.loads(subprocess.check_output([str(binary), "--version"]))}
    save(lane / "binary.json", identity)
    summary = []
    for cohort in frozen["cohorts"]:
        profile = Path(cohort["profile_dir"]) / (cohort["profile_id"] + ".json")
        if digest(profile) != cohort["profile_sha256"]:
            raise RuntimeError("Frozen profile drift")
        base = lane / cohort["id"]
        options = ["--root", cohort["root"], "--profile", cohort["profile_id"],
                   "--profile-dir", cohort["profile_dir"], "--out-dir", str(base / "index")]
        build = invoke(binary, [*options, "build"], base / "build.json")
        row = {"cohort": cohort["id"], "build_ok": build.get("ok")}
        if build.get("ok"):
            for command in ("check", "doctor", "eval", "pack-eval"):
                flags = [] if command == "check" else ["--strict"]
                value = invoke(binary, [*options, command, *flags], base / (command + ".json"))
                row[command] = {key: value[key] for key in ("ok", "summary", "query_summary") if key in value}
            row["manifest_sha256"] = digest(base / "index/docs/kb/generated/manifest.json")
        summary.append(row)
        print(json.dumps(row), flush=True)
    if digest(binary) != identity["sha256"]:
        raise RuntimeError("Binary changed during run")
    save(lane / "summary.json", summary)


def eval_rows(value):
    rows = {}
    for case in value.get("cases", []):
        for item in [case, *case.get("query_variants", [])]:
            key = case["id"] + ":" + item.get("query", "")
            rows[key] = {k: item.get(k) for k in (
                "returned_paths", "returned_record_types", "missing_paths",
                "missing_top_paths", "unexpected_paths", "rank_violations", "ceiling_violations")}
            rows[key]["passed"] = item.get("query_passed", item.get("passed"))
    return rows


def compare(output, left, right):
    frozen = json.loads((output / "frozen.json").read_text())
    report = []
    for cohort in frozen["cohorts"]:
        a_path = output / "lanes" / left / cohort["id"] / "eval.json"
        b_path = output / "lanes" / right / cohort["id"] / "eval.json"
        if not a_path.exists() or not b_path.exists():
            report.append({"cohort": cohort["id"], "compared": False})
            continue
        a, b = (eval_rows(json.loads(p.read_text())["result"]) for p in (a_path, b_path))
        changes = [{"case": key, "left": a.get(key), "right": b.get(key)}
                   for key in sorted(a.keys() | b.keys()) if a.get(key) != b.get(key)]
        publications = []
        for result_path in (a_path, b_path):
            generated = result_path.parent / "index/docs/kb/generated"
            if not (generated / "manifest.json").is_file():
                raise RuntimeError("Missing publication manifest")
            publications.append({str(p.relative_to(generated)): digest(p)
                                 for p in generated.rglob("*") if p.is_file()})
        old, new = publications
        differing = [name for name in sorted(old.keys() | new.keys()) if old.get(name) != new.get(name)]
        report.append({"cohort": cohort["id"], "compared": True, "changes": changes,
                       "publication": {"left_files": len(old), "right_files": len(new),
                                       "identical_bytes": not differing, "differing_files": differing}})
    save(output / f"comparison-{left}-{right}.json", report)
    print(json.dumps([{**row, "changes": len(row.get("changes", []))} for row in report]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("freeze").add_argument("--spec", required=True, type=Path)
    execute = sub.add_parser("run")
    execute.add_argument("--label", required=True)
    execute.add_argument("--binary", required=True, type=Path)
    comparison = sub.add_parser("compare")
    comparison.add_argument("--left", required=True)
    comparison.add_argument("--right", required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if args.command == "freeze":
        freeze(json.loads(args.spec.read_text()), output)
    elif args.command == "run":
        run(output, args.label, args.binary)
    else:
        compare(output, args.left, args.right)
