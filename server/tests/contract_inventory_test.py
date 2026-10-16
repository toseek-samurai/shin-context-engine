#!/usr/bin/env python3
"""Check current machine guidance against a real binary without a database."""

import argparse
from contextlib import contextmanager
import copy
import importlib.util
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ContractInventoryTests(unittest.TestCase):
    binary: Path

    def setUp(self) -> None:
        self.inventory = json.loads((ROOT / "docs/machine/shin-companion-protocols-v1.json").read_text())

    def invoke(self, *arguments: str, success: bool = True) -> dict:
        completed = subprocess.run([str(self.binary), *arguments], capture_output=True, text=True, timeout=5, check=False)
        self.assertEqual(completed.returncode, 0 if success else 1)
        self.assertEqual(completed.stderr if success else completed.stdout, "")
        value = json.loads(completed.stdout if success else completed.stderr)
        self.assertEqual(set(value), {"schema_version", "tool", "version", "stability", "ok", "result" if success else "error"})
        self.assertEqual(value["schema_version"], "shin.server-response.v1")
        self.assertEqual(value["tool"], "shin-server")
        self.assertEqual(value["version"], (ROOT / "server/VERSION").read_text().strip())
        self.assertEqual(value["stability"], "development")
        self.assertIs(value["ok"], success)
        return value["result" if success else "error"]

    @contextmanager
    def fixture(self):
        with tempfile.TemporaryDirectory(prefix="shin-contract-") as temporary, socket.socket() as trap:
            root = Path(temporary).resolve() / "private 'quoted' fixture"
            root.mkdir(mode=0o700)
            for name in ("allowed", "preparations"):
                (root / name).mkdir(mode=0o700)
            trap.bind(("127.0.0.1", 0))
            trap.listen()
            trap.setblocking(False)
            connection = root / "connection"
            connection.write_text(f"hostaddr=127.0.0.1 port={trap.getsockname()[1]} dbname=fixture user=fixture sslmode=disable\n")
            connection.chmod(0o600)
            def expand(value):
                if isinstance(value, str):
                    return value.replace("${ROOT}", str(root))
                if isinstance(value, dict):
                    return {key: expand(item) for key, item in value.items()}
                if isinstance(value, list):
                    return [expand(item) for item in value]
                return value
            value = expand(self.inventory["serviceConfigExample"])
            config = root / "runtime.json"
            config.touch(mode=0o600)
            yield root, config, value
            with self.assertRaises(BlockingIOError):
                accepted, _ = trap.accept()
                accepted.close()

    def test_current_inventory_matches_binary(self) -> None:
        result = self.invoke("--version")
        self.assertEqual(result["contracts"], self.inventory["contracts"])
        self.assertEqual(result["retrieval_routing"], "managed-generation-config-v10")
        self.assertIs(result["observer_reclamation_available"], True)
        self.assertIs(result["observer_unconfirmed_discard_available"], True)
        self.assertIs(result["observer_identity_retirement_available"], True)
        retrieval = json.loads((ROOT / "docs/machine/shin-server-retrieval-v1.json").read_text())
        self.assertEqual(retrieval["configuration"]["schema"], self.inventory["contracts"]["configuration_schema"])
        self.assertNotIn("author v5", retrieval["configuration"]["compatibility"])
        observability = json.loads((ROOT / "docs/machine/shin-retrieval-observability-v1.json").read_text())
        self.assertEqual(observability["currentConfigurationSchema"], self.inventory["contracts"]["configuration_schema"])
        self.assertEqual(self.inventory["serviceConfigExample"]["observability"],
                         observability["configuration"]["observability"])
        self.assertEqual(retrieval["configuration"]["observability"], observability["configuration"]["observability"])

    def test_documented_config_is_read_only_and_network_free(self) -> None:
        with self.fixture() as (root, config, value):
            config.write_text(json.dumps(value))
            before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            result = self.invoke("check-config", "--config", str(config))
            self.assertEqual(result, {"configuration_schema": "shin.server-config.v10", "valid": True,
                                     "read_only": True, "database_contacted": False, "source_mutated": False})
            self.assertEqual(before, {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()})
            self.assertEqual(list((root / "preparations").iterdir()), [])

    def test_operating_guidance_uses_current_protocol_owner(self) -> None:
        schema = self.inventory["contracts"]["configuration_schema"]
        for name in ("activation", "preparation", "retrieval"):
            with self.subTest(contract=name):
                contract = json.loads((ROOT / f"docs/machine/shin-server-{name}-v1.json").read_text())
                self.assertEqual(contract["configuration"]["schema"], schema)

        fields = (
            ("activation", ("configuration", "cutover")),
            ("preparation", ("configuration", "migration")),
            ("recovery", ("deployment", "config")),
            ("workspaces", ("routes", "config")),
            ("foundation", ("delivery",)),
            ("dev-workflow", ("journalLifecycle", "migration")),
            ("dev-workflow", ("localObserver", "version")),
        )
        for name, keys in fields:
            with self.subTest(contract=name, field=keys):
                value = json.loads((ROOT / f"docs/machine/shin-server-{name}-v1.json").read_text())
                for key in keys:
                    value = value[key]
                self.assertIn("shin-companion-protocols-v1", value)
                # Historical versions may remain; current operating claims defer to the owner.
                self.assertFalse(re.search(
                    r"shin\.server-config\.v\d+|\b[Cc]urrent (?:readers accept only (?:config )?|config )v\d+|\bSchema\d+ remains current",
                    value))

    def test_old_config_and_missing_required_fields_are_rejected(self) -> None:
        with self.fixture() as (_, config, value):
            cases = []
            for version in (5, 6, 7, 8, 9):
                old = copy.deepcopy(value)
                old["schema_version"] = f"shin.server-config.v{version}"
                cases.append((f"v{version}-with-observability", old))
                legacy = copy.deepcopy(old)
                del legacy["observability"]
                cases.append((f"v{version}", legacy))
                if version == 7:
                    without_knowledge = copy.deepcopy(old)
                    del without_knowledge["knowledge"]
                    cases.append(("v7-without-knowledge", without_knowledge))
            missing = copy.deepcopy(value)
            del missing["database"]["server_id"]
            cases.append(("missing-pin", missing))
            missing = copy.deepcopy(value)
            del missing["observability"]
            cases.append(("missing-observability", missing))
            for name, invalid in cases:
                with self.subTest(case=name):
                    config.write_text(json.dumps(invalid))
                    self.assertEqual(self.invoke("check-config", "--config", str(config), success=False)["code"], "invalid-config")


class DevConfigGenerationTests(unittest.TestCase):
    def setUp(self) -> None:
        spec = importlib.util.spec_from_file_location("shin_server_dev", ROOT / "server/tools/dev.py")
        self.dev = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.dev)

    def generated(self, root: Path) -> dict:
        return self.dev._runtime_config(root, root / "runtime.conninfo", "a" * 64, "b" * 64,
                                        "0123456789abcdef0123456789abcdef")

    def test_generation_preserves_explicit_identity_and_disables_capture(self) -> None:
        root = Path("/private/fixture")
        config = self.generated(root)
        self.assertEqual(config["schema_version"], "shin.server-config.v10")
        self.assertEqual(config["knowledge"], {
            "enabled": False,
            "principal_id": "",
            "route_ids": [],
            "interval_seconds": 60,
            "max_age_seconds": 180,
            "prepare_on_drift": False,
        })
        self.assertEqual(config["journal_writer"], {
            "enabled": False,
            "principal_id": "",
            "interval_ms": 1000,
            "max_retry_ms": 30000,
            "flush_on_shutdown": True,
        })
        contract = json.loads((ROOT / "docs/machine/shin-retrieval-observability-v1.json").read_text())
        self.assertEqual(config["observability"], contract["configuration"]["observability"])
        self.assertEqual(config["database"]["server_id"], "0123456789abcdef0123456789abcdef")
        self.assertEqual(config["database"]["connection_file"], str(root / "runtime.conninfo"))
        self.assertEqual(config["principals"][0]["token_sha256"], self.dev._sha256("a" * 64))
        self.assertEqual(config["principals"][1]["token_sha256"], self.dev._sha256("b" * 64))
        self.assertEqual(config["retrieval"]["routes"], [])

    def test_existing_configs_are_not_migrated_or_reset(self) -> None:
        with tempfile.TemporaryDirectory(prefix="shin-config-generation-") as temporary:
            root = Path(temporary).resolve()
            config = self.generated(root)
            content = json.dumps(config, sort_keys=True, separators=(",", ":")) + "\n"
            path = root / "runtime.json"
            self.dev._ensure_exact_private_file(path, content)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            before = path.stat()
            self.dev._ensure_exact_private_file(path, content)
            self.assertEqual(path.stat().st_ino, before.st_ino)
            self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(path.read_text(), content)
            for version in (5, 6, 7, 8, 9):
                with self.subTest(version=version):
                    existing = copy.deepcopy(config)
                    existing["schema_version"] = f"shin.server-config.v{version}"
                    if version < 7:
                        del existing["observability"]
                    if version < 8:
                        del existing["knowledge"]
                    if version < 9:
                        del existing["journal_writer"]
                    if version == 5:
                        del existing["database"]["server_id"]
                    existing["retrieval"]["routes"] = [{"id": "reviewed-route"}]
                    original = json.dumps(existing).encode()
                    path.write_bytes(original)
                    with self.assertRaises(self.dev.DevStateError):
                        self.dev._ensure_exact_private_file(path, content)
                    self.assertEqual(path.read_bytes(), original)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    arguments = parser.parse_args()
    ContractInventoryTests.binary = arguments.binary.resolve(strict=True)
    unittest.main(argv=[__file__])
