#!/usr/bin/env python3
"""Stage and relocate native companions without installing services or configuration."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


LICENSES = (
    "cpp-httplib/LICENSE", "nlohmann/LICENSE.MIT",
    "simdjson/LICENSE-APACHE-2.0", "simdjson/LICENSE-MIT",
    "tree-sitter/LICENSE", "tree-sitter/UPSTREAM.json", "tree-sitter-unicode/LICENSE",
    "tree-sitter-java/LICENSE", "tree-sitter-java/UPSTREAM.json",
    "tree-sitter-typescript/LICENSE", "tree-sitter-typescript/UPSTREAM.json",
    "tree-sitter-cpp/LICENSE", "tree-sitter-cpp/UPSTREAM.json",
    "pugixml/LICENSE.MIT", "pugixml/UPSTREAM.json",
)


def run(command: list[str], cwd: Path, environment: dict[str, str]) -> str:
    result = subprocess.run(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=60, check=False)
    if result.returncode:
        raise RuntimeError(f"{Path(command[0]).name} exited {result.returncode}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--server-build", type=Path, required=True)
    parser.add_argument("--observer-build", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    report_path = args.report.resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": "shin.companion-install-check.v1", "ok": False,
              "components": [], "external_runtime_libraries_required": True,
              "dynamic_loader_override_present": any(os.environ.get(key) for key in
                  ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH")),
              "service_installation_tested": False, "source_tree_hidden": False}
    stage = "setup"
    try:
        with tempfile.TemporaryDirectory(prefix="companion-install-", dir=report_path.parent) as temporary:
            root = Path(temporary)
            environment = dict(os.environ)
            home = root / "home"
            home.mkdir()
            environment["HOME"] = str(home)
            for name, build in (("shin-server", args.server_build), ("shin-observer", args.observer_build)):
                build = build.resolve(strict=True)
                stage = f"{name}-reference"
                original = json.loads(run([str(build / name), "--version"], root, environment))
                if original.get("ok") is not True or original.get("tool") != name:
                    raise RuntimeError("invalid reference identity")
                prefix = root / name
                stage = f"{name}-install"
                run([args.cmake, "--install", str(build), "--prefix", str(prefix)], root, environment)
                moved = root / f"{name}-relocated"
                prefix.rename(moved)
                stage = f"{name}-inventory"
                if {entry.name for entry in moved.iterdir()} != {"bin", "share"}:
                    raise RuntimeError("dependency SDK leaked into companion runtime installation")
                share = moved / "share" / name
                if (share / "VERSION").read_text(encoding="utf-8").strip() != original["version"]:
                    raise RuntimeError("installed VERSION differs from binary")
                expected = LICENSES + (("libpqxx/COPYING",) if name == "shin-server" else ())
                for relative in expected:
                    notice = share / "licenses" / relative
                    if not notice.is_file() or notice.stat().st_size == 0:
                        raise RuntimeError(f"missing license inventory: {relative}")
                    if notice.suffix == ".json":
                        json.loads(notice.read_text(encoding="utf-8"))
                stage = f"{name}-relocated-version"
                binary = moved / "bin" / name
                installed = json.loads(run([str(binary), "--version"], root, environment))
                if installed != original:
                    raise RuntimeError("relocated binary identity differs from reference")
                report["components"].append({"tool": name, "version": installed["version"],
                    "reference_sha256": hashlib.sha256((build / name).read_bytes()).hexdigest(),
                    "installed_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    "license_files": len(expected), "relocated": True, "version_envelope_equal": True})
            stage = "home-preservation"
            if any(home.iterdir()):
                raise RuntimeError("version probe wrote into temporary HOME")
        report["ok"] = True
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report["failure"] = {"stage": stage, "type": type(error).__name__}
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"ok": report["ok"], "report": str(report_path)}))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
