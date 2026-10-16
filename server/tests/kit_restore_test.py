#!/usr/bin/env python3
"""Restore a prior matched kit's backup without cross-Core data transplantation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
from types import SimpleNamespace

from backup_restore_checks import inventory, verify_inventory
from headless_workflow_test import (IntegrationSuite, PROJECT, ROUTE, ROUTE_URL,
                                    WorkflowProcess, build_path, prepare_fixture,
                                    read_json, require, run_suite, write_json, write_text)
from integration_test import REPOSITORY_ROOT

sys.path.insert(0, str(REPOSITORY_ROOT / "server/tools"))
from candidate_kit import digest, verify


def validate_components(components):
    require(set(components) == {"shin-context", "shin-server", "shin-observer"},
            "kit must contain exactly the three companion identities")
    core = components["shin-context"]["envelope"]["version"]
    for name, component in components.items():
        envelope = component["envelope"]
        require(envelope.get("ok") is True and envelope.get("tool") == name,
                "kit component identity is invalid")
        if name != "shin-context":
            embedded = envelope.get("core_version", envelope.get("result", {}).get("core_version"))
            require(embedded == core, "kit embeds mismatched Core versions")
    require(components["shin-server"]["envelope"]["result"]["contracts"]["configuration_schema"]
            == "shin.server-config.v10", "unsupported fixture configuration schema")


def load_kit(prefix):
    prefix = prefix.resolve(strict=True)
    manifest_path = prefix / "candidate-manifest.json"
    require(manifest_path.is_file() and not manifest_path.is_symlink(), "missing kit manifest")
    manifest = read_json(manifest_path)
    require(manifest.get("schema_version") == "shin.candidate-kit.v1", "unsupported kit manifest")
    verify(prefix, manifest)
    validate_components(manifest["components"])
    for name, component in manifest["components"].items():
        binary = prefix / "bin" / name
        require(binary.is_file() and not binary.is_symlink() and os.access(binary, os.X_OK),
                "kit executable missing or unsafe")
        require(digest(binary) == component["installed_sha256"], "kit executable hash differs")
        actual = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=30)
        require(actual.returncode == 0 and json.loads(actual.stdout) == component["envelope"],
                "kit executable envelope differs from manifest")
    return {"prefix": prefix, "manifest": manifest, "manifest_sha256": digest(manifest_path)}


def verify_kit(kit):
    require(digest(kit["prefix"] / "candidate-manifest.json") == kit["manifest_sha256"],
            "kit manifest changed during run")
    verify(kit["prefix"], kit["manifest"])


def database_fingerprint(suite, name):
    require(re.fullmatch(r"shin_candidate_[0-9a-f]{16}", name) is not None,
            "database is outside the disposable candidate namespace")
    dump = suite.compose("exec", "-T", "postgres", "pg_dump", "--format=plain",
                         "--schema=shin_server", "--no-owner", "--no-privileges",
                         "-U", "shin_migrator", "-d", name, timeout=90).stdout
    require(bool(dump), "candidate database dump is empty")
    normalized = re.sub(r"^\\(?:un)?restrict [^\r\n]*\r?\n", "", dump, flags=re.MULTILINE)
    return hashlib.sha256(normalized.encode()).hexdigest()


def connection_value(database, role, password, port=15432):
    require(re.fullmatch(r"shin_candidate_[0-9a-f]{16}", database) is not None
            and role in ("runtime", "migrator")
            and re.fullmatch(r"[A-Za-z0-9_-]{32,128}", password) is not None
            and type(port) is int and 1 <= port <= 65535,
            "unsafe disposable database connection value")
    return (f"host=127.0.0.1 port={port} dbname={database} user=shin_{role} "
            f"password={password} gssencmode=disable\n")


class CandidateTrial:
    def __init__(self, kit):
        self.kit = kit
        self.database = "shin_candidate_" + secrets.token_hex(8)
        self.created = False
        self.report = {"ok": False, "checks": []}

    def run(self, suite, previous_worker):
        require(suite.server is None, "previous writer must be stopped before candidate trial")
        old_binary, old_version = suite.binary, suite.expected_server_version
        paths = [suite.state_root / "config" / f"{role}.json" for role in ("runtime", "migrator")]
        originals = {path: path.read_bytes() for path in paths}
        self.home = previous_worker.home / "candidate-trial"
        self.home.mkdir(mode=0o700)
        prefix = self.kit["prefix"]
        pin = secrets.token_hex(16)
        try:
            suite.compose("exec", "-T", "postgres", "createdb", "-U", "shin_migrator",
                          "--template=template0", self.database)
            self.created = True
            for role, token in (("runtime", "shin_runtime_password"), ("migrator", "postgres_migrator_password")):
                path = suite.state_root / "config" / f"{role}.json"
                connection = self.home / f"{role}.conninfo"
                write_text(connection, connection_value(self.database, role, suite.read_private_token(token), suite.postgres_port()))
                config = json.loads(originals[path])
                config["database"].update(expected_name=self.database, connection_file=str(connection), server_id=pin)
                config["allowed_roots"] = [str(self.home / "source")]
                config["retrieval"]["routes"] = [{"id": ROUTE, "project_id": PROJECT, "workspace_id": "primary",
                    "root": str(self.home / "source"), "profile": "generic-docs", "profile_dir": str(self.home / "profiles")}]
                config["preparations"]["state_dir"] = str(self.home / "managed")
                write_json(path, config)
            prepare_fixture(self.home, prefix / "bin/shin-context", prefix / "bin/shin-observer", suite,
                            prefix / "share/shin-candidate/agent", prefix / "share/shin-context/profiles/generic-docs.json")
            # Distinguish candidate-only data from the previous backup's source.
            with (self.home / "source/docs/policy.md").open("a") as output:
                output.write("\nCandidate-only policy revision must not enter the restored previous kit.\n")
            suite.binary = prefix / "bin/shin-server"
            suite.expected_server_version = self.kit["manifest"]["components"]["shin-server"]["envelope"]["version"]
            suite.check_version()
            suite.migrate()
            suite.start_server()
            worker = WorkflowProcess(self.home)
            worker.attach()
            worker.capture()
            current = worker.http("POST", ROUTE_URL + "/brief", {"query": "publication authorization", "require_current": True})
            self.report.update(candidate_snapshot_id=current["snapshot_id"], workflow_checks=worker.checks)
            for command in ("sample", "status"):
                result = subprocess.run([str(prefix / "bin/shin-observer"), command, "--config",
                                         str(self.home / "observer.json")], capture_output=True, text=True, timeout=30)
                require(result.returncode == 0 and json.loads(result.stdout)["ok"], "candidate Observer failed")
            suite.stop_server(expect_clean=True)
            self.fingerprint = database_fingerprint(suite, self.database)
            self.protected = {name: inventory(self.home / name) for name in
                              ("source", "profiles", "managed", "local-out", "state", "observer-state")}
            self.report["checks"].append("current-kit-ran-after-old-backup-on-separate-database-and-artifacts")
        finally:
            try:
                suite.stop_server(expect_clean=True)
            finally:
                suite.binary, suite.expected_server_version = old_binary, old_version
                for path, raw in originals.items():
                    write_text(path, raw.decode())
        require(all(path.read_bytes() == raw for path, raw in originals.items()),
                "previous configuration was not restored before old-kit recovery")

    def verify_preserved(self, suite):
        require(database_fingerprint(suite, self.database) == self.fingerprint,
                "previous-kit restore mutated candidate database")
        for name, expected in self.protected.items():
            verify_inventory(self.home / name, expected)
        verify_kit(self.kit)
        self.report["checks"].append("previous-kit-restore-preserved-current-kit-database-source-checkpoints-and-artifacts")
        self.report["candidate_database_sha256"] = self.fingerprint
        self.report["ok"] = True

    def cleanup(self, suite):
        if self.created:
            require(re.fullmatch(r"shin_candidate_[0-9a-f]{16}", self.database) is not None,
                    "refusing cleanup outside the disposable candidate namespace")
            suite.compose("exec", "-T", "postgres", "dropdb", "-U", "shin_migrator", self.database)
            self.created = False
        self.report["temporary_database_removed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("previous-prefix", "candidate-prefix", "state-root", "report"):
        parser.add_argument("--" + name, type=build_path, required=True)
    args = parser.parse_args()
    require(not args.report.exists(), "refusing an existing report")
    workflow_report = args.report.with_name(args.report.stem + "-workflow.json")
    require(not workflow_report.exists(), "refusing an existing workflow receipt")
    previous, candidate = load_kit(args.previous_prefix), load_kit(args.candidate_prefix)
    old = previous["manifest"]["components"]
    new = candidate["manifest"]["components"]
    require(old["shin-context"]["envelope"]["version"] != new["shin-context"]["envelope"]["version"],
            "this rehearsal requires different Core generations, not a fabricated compatible control")
    require(old["shin-server"]["envelope"]["result"]["contracts"] == new["shin-server"]["envelope"]["result"]["contracts"],
            "this schema019 fixture does not qualify different server protocols")
    trial = CandidateTrial(candidate)

    class MatchedKitSuite(IntegrationSuite):
        def check_version(self):
            choices = {kit["prefix"] / "bin/shin-server": kit for kit in (previous, candidate)}
            require(self.binary in choices, "Server does not belong to an admitted kit")
            selected = choices[self.binary]["manifest"]["components"]["shin-server"]
            actual = self.run_process([str(self.binary), "--version"], "matched-kit version").stdout
            require(json.loads(actual) == selected["envelope"] and digest(self.binary) == selected["installed_sha256"],
                    "selected Server differs from its admitted kit")
            self.assert_envelope(json.loads(actual))

    def previous_suite(binary, state_root, report):
        suite = MatchedKitSuite(binary, state_root, report)
        suite.expected_server_version = old["shin-server"]["envelope"]["version"]
        return suite

    selected = SimpleNamespace(core=args.previous_prefix / "bin/shin-context", server=args.previous_prefix / "bin/shin-server",
        observer=args.previous_prefix / "bin/shin-observer", installed_prefix=args.previous_prefix,
        state_root=args.state_root, report=workflow_report, agent_workflow=False, hermes=None,
        rollback_server=None, backup_restore=True)
    code = run_suite(selected, suite_factory=previous_suite, candidate_trial=trial)
    verify_kit(previous)
    verify_kit(candidate)
    result = read_json(workflow_report)
    if result["ok"]:
        require(result["backup_restore"]["metrics"]["snapshot_id"] != trial.report["candidate_snapshot_id"],
                "restored previous source was not distinguished from candidate-only source")
    result["schema_version"] = "shin.full-kit-restore-report.v1"
    result["workflow_receipt"] = {"path": str(workflow_report), "sha256": digest(workflow_report)}
    result["full_kit_restore"] = {"previous_manifest_sha256": previous["manifest_sha256"],
        "candidate_manifest_sha256": candidate["manifest_sha256"],
        "previous_components": old, "candidate_components": new, "candidate_trial": trial.report,
        "cross_core_in_place_rollback_allowed": False,
        "boundary": "Synthetic previous-kit backup, current kit on independent data, then previous-kit restore to a new database. No mixed-kit replay, in-place downgrade, production cutover or post-backup-data merge."}
    result["ok"] = result["ok"] and trial.report["ok"] and trial.report.get("temporary_database_removed") is True
    write_json(args.report, result)
    print(json.dumps({"ok": result["ok"], "report": str(args.report), "full_kit_restore": trial.report}))
    return code or int(not result["ok"])


if __name__ == "__main__":
    raise SystemExit(main())
