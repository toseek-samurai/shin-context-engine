#!/usr/bin/env python3
"""Assemble a native development kit without installing services or credentials."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = "candidate-manifest.json"


def require(value, message):
    if not value:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def inventory(root):
    rows = []
    for path in sorted(root.rglob("*")):
        info = path.lstat()
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode), "special file in candidate kit")
        if path.is_file() and path != root / MANIFEST:
            rows.append({"path": path.relative_to(root).as_posix(), "bytes": info.st_size,
                         "mode": stat.S_IMODE(info.st_mode), "sha256": digest(path)})
    return rows


def runtime_layout(root):
    require({entry.name for entry in root.iterdir()} == {"bin", "share"},
            "unexpected runtime payload (possibly a dependency SDK)")


def library_lines(output, system):
    lines = output.splitlines()
    require("not found" not in output, "unresolved runtime library")
    return lines[1:] if system == "Darwin" else lines


def verify(root, manifest):
    require(inventory(root) == manifest["files"], "candidate inventory differs")


def core_allocator(prefix):
    policy = prefix / "share/shin-context/shin-core-allocator.json"
    if not policy.exists():
        return None
    value = read_json(policy)
    expected = {"schema_version": "shin.core-allocator.v1", "id": "mimalloc", "version": "3.5.3",
                "scope": "standalone-linux-core-cli", "linkage": "static-object",
                "transparent_huge_pages": False,
                "source_sha256": "3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47"}
    require(value == expected and type(value.get("transparent_huge_pages")) is bool,
            "unknown Core allocator policy")
    require(platform.system() == "Linux", "mimalloc Core package requires Linux")
    require((prefix / "share/shin-context/licenses/mimalloc/LICENSE").stat().st_size > 0,
            "missing mimalloc license")
    return value


def run(command, home):
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("LD_", "DYLD_"))}
    environment["HOME"] = str(home)
    result = subprocess.run(command, cwd=home, env=environment, capture_output=True,
                            text=True, timeout=120, check=False)
    require(result.returncode == 0, f"{Path(command[0]).name} failed with exit {result.returncode}")
    return result.stdout


def expand(value, replacements):
    if isinstance(value, str):
        for key, replacement in replacements.items():
            value = value.replace("${" + key + "}", str(replacement))
        return value
    if isinstance(value, dict):
        return {key: expand(item, replacements) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, replacements) for item in value]
    return value


def check_templates(prefix, home):
    fixture = home / "fixture"
    fixture.mkdir()
    for name in ("allowed", "preparations", "source", "index", "observer-state"):
        (fixture / name).mkdir()
    (fixture / "source/README.md").write_text("# Package Fixture\n\nLocal package verification.\n")
    (fixture / "connection").write_text("hostaddr=127.0.0.1 port=1 dbname=fixture user=fixture sslmode=disable\n")
    templates = prefix / "share/shin-candidate/templates"
    replacements = {"ROOT": fixture, "PREFIX": prefix}
    for name in ("server-config", "observer-config"):
        write_json(fixture / f"{name}.json", expand(read_json(templates / f"{name}.json"), replacements))
    server = json.loads(run([str(prefix / "bin/shin-server"), "check-config", "--config",
                             str(fixture / "server-config.json")], home))
    require(server["ok"] and server["result"]["database_contacted"] is False,
            "server template must validate without a database")
    run([str(prefix / "bin/shin-context"), "--root", str(fixture / "source"), "--out-dir",
         str(fixture / "index"), "--profile", "generic-docs", "--profile-dir",
         str(prefix / "share/shin-context/profiles"), "build"], home)
    for command in ("sample", "status"):
        value = json.loads(run([str(prefix / "bin/shin-observer"), command, "--config",
                                str(fixture / "observer-config.json")], home))
        require(value["ok"], "Observer template probe failed")
    return ["server-check-config-no-database", "offline-core-build", "observer-sample", "observer-status"]


def assemble(args):
    output = args.output.absolute()
    require(output.parent.resolve().is_relative_to((ROOT / "build").resolve()),
            "candidate output must be inside this checkout's build tree")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir(mode=0o700)  # Refuse existing destinations, including symlinks.
    with tempfile.TemporaryDirectory(prefix=".assembling-", dir=output) as temporary:
        temporary = Path(temporary)
        home = temporary / "home"
        home.mkdir()
        prefix = temporary / "stage"
        identities = {}
        builds = {"shin-context": args.core_build, "shin-server": args.server_build,
                  "shin-observer": args.observer_build}
        versions = {"shin-context": ROOT / "VERSION", "shin-server": ROOT / "server/VERSION",
                    "shin-observer": ROOT / "observer/VERSION"}
        for name, build in builds.items():
            build = build.resolve(strict=True)
            value = json.loads(run([str(build / name), "--version"], home))
            require(value.get("ok") is True and value.get("tool") == name and
                    value["version"] == versions[name].read_text().strip(), "binary/source identity mismatch")
            identities[name] = {"envelope": value, "source_sha256": digest(build / name)}
            run([args.cmake, "--install", str(build), "--prefix", str(prefix)], home)
        for name in ("shin-server", "shin-observer"):
            value = identities[name]["envelope"]
            core_version = value.get("core_version", value.get("result", {}).get("core_version"))
            require(core_version == identities["shin-context"]["envelope"]["version"], "embedded Core mismatch")
        relocated = temporary / "relocated"
        prefix.rename(relocated)
        prefix = relocated
        runtime_layout(prefix)
        for name in builds:
            require(json.loads(run([str(prefix / "bin" / name), "--version"], home)) ==
                    identities[name]["envelope"], "relocated identity mismatch")
            require((prefix / "share" / name / "VERSION").read_text().strip() ==
                    identities[name]["envelope"]["version"], "installed VERSION mismatch")
            identities[name]["installed_sha256"] = digest(prefix / "bin" / name)
            require(digest(builds[name].resolve() / name) == identities[name]["source_sha256"],
                    "source binary changed during assembly")
        require(not list(home.iterdir()), "version probes mutated HOME")
        share = prefix / "share/shin-candidate"
        share.mkdir()
        # Shipping the small contract library preserves references; it is not auto-indexed.
        contracts = share / "contracts"
        contracts.mkdir()
        for path in sorted((ROOT / "docs/machine").glob("*.json")):
            require(not path.is_symlink(), "symlink in source contracts")
            read_json(path)
            shutil.copyfile(path, contracts / path.name)
        agent = share / "agent"
        agent.mkdir()
        for name in ("AGENT-WORKFLOW.md", "workflow-examples.json", "INSTALLATION.md"):
            shutil.copyfile(ROOT / "integrations/shin-server" / name, agent / name)
        templates = share / "templates"
        templates.mkdir()
        protocols = read_json(contracts / "shin-companion-protocols-v1.json")
        require(identities["shin-server"]["envelope"]["result"]["contracts"] == protocols["contracts"],
                "Server protocol inventory differs")
        write_json(templates / "server-config.json", protocols["serviceConfigExample"])
        write_json(templates / "observer-delivery.json",
                   read_json(contracts / "shin-observer-delivery-v1.json")["configuration"]["example"])
        shutil.copyfile(ROOT / "integrations/shin-server/observer-config.template.json",
                        templates / "observer-config.json")
        required = read_json(contracts / "portable-distribution-v1.json")["layout"]["thirdPartyLicenses"]
        for name in builds:
            notices = list(required)
            if name != "shin-context":
                notices = [p.replace("tree-sitter/unicode", "tree-sitter-unicode") for p in notices]
                notices.append("licenses/cpp-httplib/LICENSE")
            if name == "shin-server":
                notices.append("licenses/libpqxx/COPYING")
            for relative in notices:
                require((prefix / "share" / name / relative).stat().st_size > 0, "missing license notice")
        checks = check_templates(prefix, home)
        allocator = core_allocator(prefix)
        inspector = "/usr/bin/otool" if platform.system() == "Darwin" else shutil.which("ldd")
        require(inspector, "dynamic library inspector unavailable")
        libraries = {name: library_lines(run([inspector, *(["-L"] if platform.system() == "Darwin" else []),
                                              str(prefix / "bin" / name)], home), platform.system())
                     for name in builds}
        manifest = {"schema_version": "shin.candidate-kit.v1", "release_approved": False,
                    "core_allocator": allocator,
                    "platform": platform.system(), "architecture": platform.machine(),
                    "components": identities, "database_schema": protocols["databaseSchema"],
                    "protocols": protocols["contracts"], "observer_formats": protocols["observerFormats"],
                    "template_checks": checks, "external_libraries": libraries,
                    "system_libraries_bundled": False, "files": inventory(prefix)}
        write_json(prefix / MANIFEST, manifest)
        verify(prefix, read_json(prefix / MANIFEST))
        prefix.rename(output / "prefix")
        verify(output / "prefix", manifest)
    return {"ok": True, "prefix": str(output / "prefix"), "files": len(manifest["files"]),
            "manifest_sha256": digest(output / "prefix" / MANIFEST), "release_approved": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("core-build", "server-build", "observer-build", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    os.umask(0o077)
    args = parser.parse_args()
    try:
        result = assemble(args)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(json.dumps({"ok": False, "error_type": type(error).__name__, "error": str(error)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
