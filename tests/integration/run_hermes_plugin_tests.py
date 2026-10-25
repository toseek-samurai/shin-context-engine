#!/usr/bin/env python3
"""Portable contract tests for the Hermes Shin adapter."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


def load_plugin(source_root: Path):
    plugin_dir = source_root / "integrations" / "hermes" / "plugins" / "shin-context"
    spec = importlib.util.spec_from_file_location(
        "shin_context_hermes_plugin",
        plugin_dir / "__init__.py",
        submodule_search_locations=[str(plugin_dir)],
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the Hermes plugin package")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FakeContext:
    def __init__(self) -> None:
        self.tools = []
        self.skills = []

    def register_tool(self, **kwargs) -> None:
        self.tools.append(kwargs)

    def register_skill(self, *args) -> None:
        self.skills.append(args)


class HermesPluginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if len(sys.argv) != 2:
            raise RuntimeError("usage: run_hermes_plugin_tests.py <source-root>")
        cls.source_root = Path(sys.argv[1]).resolve()
        cls.plugin = load_plugin(cls.source_root)

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="adapter-contract-test-")
        self.base = Path(self.temporary.name)
        self.root = self.base / "sample-project"
        self.profile_dir = self.base / "profiles"
        self.cache_dir = self.base / "cache"
        self.out_dir = self.cache_dir / "fixture-index"
        self.marker = self.root / "docs" / "machine" / "shin-project-adoption-v1.json"
        self.binary = self.base / "context-tool-fake"

        self.marker.parent.mkdir(parents=True)
        self.profile_dir.mkdir()
        (self.out_dir / "docs" / "kb" / "generated").mkdir(parents=True)
        (self.root / "src").mkdir()
        (self.profile_dir / "fixture-profile.json").write_text("{}\n", encoding="utf-8")
        (self.out_dir / "docs" / "kb" / "generated" / "manifest.json").write_text(
            "{}\n", encoding="utf-8"
        )
        self.marker.write_text(
            json.dumps(
                {
                    "schemaVersion": "shin.project-adoption.v1",
                    "status": "active",
                    "profile": {"id": "fixture-profile"},
                    "index": {"key": "fixture-index"},
                }
            ),
            encoding="utf-8",
        )
        self.binary.write_text(
            """#!/usr/bin/env python3
import json
import os
import sys

args = sys.argv[1:]
known = {
    "discover", "ready", "brief", "pack", "diagnose", "worktree-diff",
    "suggest-profile"
}
command = next((item for item in args if item in known), "unknown")
log_path = os.environ.get("SHIN_FAKE_CALL_LOG")
if log_path:
    with open(log_path, "a", encoding="utf-8") as stream:
        stream.write(command + "\\n")
schemas = {
    "discover": "shin.discovery.v1",
    "ready": "shin.agent-readiness.v1",
    "brief": "shin.context-brief-compact.v1",
    "pack": "shin.agent-context-pack-compact.v1",
    "diagnose": "shin.diagnose.v1",
    "worktree-diff": "shin.worktree-diff.v1",
    "suggest-profile": "shin.profile-suggestion.v1",
}
mode = os.environ.get("SHIN_FAKE_MODE")
if command == "ready" and mode == "ready-fail":
    print(json.dumps({
        "schema_version": schemas[command],
        "tool": "shin-context",
        "command": command,
        "ok": False,
        "next_action": "rebuild-index",
    }))
    raise SystemExit(7)
if mode in {
    "stderr-error",
    "stderr-error-wrong-command",
    "stderr-error-wrong-schema",
    "stderr-error-wrong-tool",
    "stderr-error-ok-string",
}:
    error_payload = {
        "schema_version": "shin.error.v1",
        "tool": "shin-context",
        "command": command,
        "ok": False,
        "error": {
            "code": "invalid-option-value",
            "category": "usage",
            "message": "The selected option is invalid.",
            "exit_code": 2,
            "retryable": False,
            "remediation": "Correct the option and retry.",
        },
    }
    if mode == "stderr-error-wrong-command":
        error_payload["command"] = "wrong-command"
    elif mode == "stderr-error-wrong-schema":
        error_payload["schema_version"] = "shin.wrong.v1"
    elif mode == "stderr-error-wrong-tool":
        error_payload["tool"] = "wrong-tool"
    elif mode == "stderr-error-ok-string":
        error_payload["ok"] = "false"
    print(json.dumps(error_payload), file=sys.stderr)
    raise SystemExit(2)
if mode == "ready-malformed" and command == "ready":
    print("{}")
    raise SystemExit(0)
if mode == "duplicate-key":
    print(
        '{"schema_version":"shin.discovery.v1","schema_version":"shin.discovery.v1",'
        '"tool":"shin-context","command":"discover","ok":true}'
    )
    raise SystemExit(0)
if mode == "non-finite":
    print(
        '{"schema_version":"shin.discovery.v1","tool":"shin-context",'
        '"command":"discover","ok":true,"diagnostic":NaN}'
    )
    raise SystemExit(0)
if mode == "stderr-duplicate-key":
    print(
        '{"schema_version":"shin.error.v1","schema_version":"shin.error.v1",'
        '"tool":"shin-context","command":"discover","ok":false}',
        file=sys.stderr,
    )
    raise SystemExit(2)
if mode == "stderr-non-finite":
    print(
        '{"schema_version":"shin.error.v1","tool":"shin-context",'
        '"command":"discover","ok":false,"diagnostic":Infinity}',
        file=sys.stderr,
    )
    raise SystemExit(2)
if mode == "stderr-too-large":
    sys.stderr.write("x" * (2 * 1024 * 1024 + 1))
    raise SystemExit(2)
payload = {
    "schema_version": schemas[command],
    "tool": "shin-context",
    "ok": True,
    "command": command,
    "argv": args,
}
if mode == "malformed-success":
    payload = {}
elif mode == "ok-string":
    payload["ok"] = "true"
elif mode == "wrong-command":
    payload["command"] = "wrong-command"
elif mode == "wrong-schema":
    payload["schema_version"] = "shin.wrong.v1"
if command == "pack":
    request_path = args[args.index("--input") + 1]
    with open(request_path, encoding="utf-8") as stream:
        payload["task_request"] = json.load(stream)
print(json.dumps(payload))
""",
            encoding="utf-8",
        )
        self.binary.chmod(0o755)
        self.call_log = self.base / "fake-calls.log"
        self.environment = {
            "SHIN_CONTEXT_BIN": str(self.binary),
            "SHIN_CONTEXT_PROFILE_DIR": str(self.profile_dir),
            "SHIN_CONTEXT_CACHE_DIR": str(self.cache_dir),
            "SHIN_FAKE_CALL_LOG": str(self.call_log),
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def invoke(self, handler, args, extra_environment=None):
        environment = dict(self.environment)
        environment.update(extra_environment or {})
        with patch.dict(os.environ, environment, clear=False):
            return json.loads(handler(args))

    def test_registers_one_toolset_and_read_only_skill(self) -> None:
        context = FakeContext()
        self.plugin.register(context)
        names = [entry["name"] for entry in context.tools]
        self.assertEqual(
            names,
            [
                "shin_discover_project",
                "shin_ready_context",
                "shin_brief_context",
                "shin_pack_context",
                "shin_diagnose_context",
                "shin_worktree_diff",
                "shin_suggest_profile",
            ],
        )
        self.assertTrue(
            all(entry["toolset"] == "plugin_shin_context" for entry in context.tools)
        )
        self.assertEqual(context.skills[0][0], "workflow")
        self.assertTrue(Path(context.skills[0][1]).is_file())
        relocated = (self.base / "installed-workflow").resolve()
        shutil.copytree(Path(context.skills[0][1]).parent, relocated)
        workflow = (relocated / "SKILL.md").read_text(encoding="utf-8")
        for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", workflow):
            if "://" in target or target.startswith("#"):
                continue
            reference = (relocated / target.split("#", 1)[0]).resolve()
            self.assertTrue(reference.is_relative_to(relocated), target)
            self.assertTrue(reference.is_file(), target)
        self.assertIn("<shin-home>/docs/machine", workflow)
        self.assertIn("<prefix>/share/shin-context/contracts", workflow)
        for name in ("execution-evidence-v1.json", "trust-boundary-hardening-v1.json"):
            self.assertIn(name, workflow)
            self.assertTrue((self.source_root / "docs" / "machine" / name).is_file())

    def test_plugin_manifest_names_the_adapter(self) -> None:
        manifest = self.source_root / "integrations" / "hermes" / "plugins" / "shin-context" / "plugin.yaml"
        self.assertIn("name: shin-context", manifest.read_text(encoding="utf-8"))

    def test_install_denies_builtin_tool_override_noninteractively(self) -> None:
        makefile = (self.source_root / "Makefile").read_text(encoding="utf-8")
        self.assertIn("plugins enable --no-allow-tool-override shin-context", makefile)

    def test_discover_is_read_only_and_structured(self) -> None:
        result = self.invoke(
            self.plugin.adapter.discover_project, {"root": str(self.root)}
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["operation"], "discover")
        self.assertEqual(result["result"]["command"], "discover")

    def test_ready_resolves_marker_profile_and_external_index(self) -> None:
        result = self.invoke(
            self.plugin.adapter.ready_context, {"root": str(self.root)}
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["route"]["profile"], "fixture-profile")
        self.assertEqual(result["route"]["index_key"], "fixture-index")
        self.assertEqual(result["route"]["out_dir"], str(self.out_dir.resolve()))
        self.assertEqual(result["result"]["command"], "ready")

    def test_brief_requires_readiness_and_never_uses_a_shell(self) -> None:
        sentinel = self.base / "must-not-exist"
        query = f"operator query; touch {sentinel}"
        result = self.invoke(
            self.plugin.adapter.brief_context,
            {"root": str(self.root), "query": query},
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["readiness"]["command"], "ready")
        self.assertEqual(result["result"]["command"], "brief")
        self.assertIn(query, result["result"]["argv"])
        self.assertFalse(sentinel.exists())

    def run_simple_cli(self, *arguments, mode=None):
        environment = {**os.environ, **self.environment}
        if mode:
            environment["SHIN_FAKE_MODE"] = mode
        return subprocess.run(
            [sys.executable, str(self.source_root / "tools" / "shin.py"), *arguments],
            cwd=self.root, env=environment, capture_output=True, text=True, timeout=30,
        )

    def test_simple_cli_uses_current_root_and_existing_ready_brief(self) -> None:
        sentinel = self.base / "must-not-exist"
        query = f"query; touch {sentinel}"
        completed = self.run_simple_cli(query, "--projection", "audit")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["route"]["root"], str(self.root.resolve()))
        self.assertIn(query, result["result"]["argv"])
        self.assertIn("--require-current", result["result"]["argv"])
        self.assertEqual(self.call_log.read_text().splitlines(), ["ready", "brief"])
        self.assertFalse(sentinel.exists())

    def test_simple_cli_stops_on_unready_route(self) -> None:
        completed = self.run_simple_cli("question", mode="ready-fail")
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)["ok"])
        self.assertEqual(self.call_log.read_text().splitlines(), ["ready"])

    def test_simple_cli_status_does_not_retrieve(self) -> None:
        completed = self.run_simple_cli("--status", "--root", str(self.root))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["operation"], "ready")
        self.assertEqual(self.call_log.read_text().splitlines(), ["ready"])

    def test_simple_cli_task_reuses_adopted_ready_pack(self) -> None:
        request = {"schema_version": "shin.task-request.v1", "request_id": "client-task",
                   "task": {"objective": "Inspect fixture context", "scope": [], "should": [],
                            "must": [{"id": "fixture", "query": "fixture contract", "require_full_coverage": True}]},
                   "budget": {"max_evidence_bytes": 8192, "max_records": 3,
                              "per_query_records": 3, "per_document_records": 3}}
        path = self.base / "client task.json"
        path.write_text(json.dumps(request))
        completed = self.run_simple_cli("--task", str(path))
        self.assertEqual(completed.returncode, 0, (completed.stdout, completed.stderr))
        result = json.loads(completed.stdout)
        self.assertEqual(result["operation"], "pack")
        self.assertEqual(result["result"]["task_request"], request)
        self.assertEqual(result["route"]["root"], str(self.root.resolve()))
        self.assertEqual(self.call_log.read_text().splitlines(), ["ready", "pack"])

    def test_simple_cli_task_never_bypasses_failed_readiness(self) -> None:
        request = {"schema_version": "shin.task-request.v1", "request_id": "client-task",
                   "task": {"objective": "Inspect fixture context", "scope": [], "should": [],
                            "must": [{"id": "fixture", "query": "fixture contract"}]},
                   "budget": {"max_evidence_bytes": 8192, "max_records": 3,
                              "per_query_records": 3, "per_document_records": 3}}
        path = self.base / "client task.json"
        path.write_text(json.dumps(request))
        completed = self.run_simple_cli("--task", str(path), mode="ready-fail")
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)["ok"])
        self.assertEqual(self.call_log.read_text().splitlines(), ["ready"])

    def test_simple_cli_never_adopts_or_switches_route(self) -> None:
        self.marker.unlink()
        completed = self.run_simple_cli("question")
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(json.loads(completed.stdout)["error"]["code"], "ADOPTION_MARKER_NOT_FOUND")
        self.assertFalse(self.call_log.exists())

    def test_simple_cli_validates_query_status_and_no_admin_flags(self) -> None:
        for arguments in ([], ["--status", "question"], ["--build"], ["--profile", "other", "query"]):
            with self.subTest(arguments=arguments):
                completed = self.run_simple_cli(*arguments)
                self.assertEqual(completed.returncode, 2)
                self.assertFalse(self.call_log.exists())

    def test_simple_cli_preserves_explicit_route_override_and_rejects_traversal(self) -> None:
        completed = self.run_simple_cli("query", "--out-dir", str(self.out_dir), "--projection", "audit")
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(json.loads(completed.stdout)["route"]["out_dir"], str(self.out_dir.resolve()))
        self.call_log.unlink()
        completed = self.run_simple_cli("query", "--marker", "../outside.json")
        self.assertEqual(completed.returncode, 1)
        self.assertEqual(json.loads(completed.stdout)["error"]["code"], "MARKER_OUTSIDE_ROOT")
        self.assertFalse(self.call_log.exists())

    def test_failed_readiness_stops_retrieval(self) -> None:
        result = self.invoke(
            self.plugin.adapter.brief_context,
            {"root": str(self.root), "query": "bounded query"},
            {"SHIN_FAKE_MODE": "ready-fail"},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["result"]["command"], "ready")
        self.assertEqual(result["process"]["exit_code"], 7)

    def test_malformed_success_envelopes_fail_closed(self) -> None:
        for mode in (
            "malformed-success",
            "ok-string",
            "wrong-command",
            "wrong-schema",
            "duplicate-key",
            "non-finite",
        ):
            with self.subTest(mode=mode):
                result = self.invoke(
                    self.plugin.adapter.discover_project,
                    {"root": str(self.root)},
                    {"SHIN_FAKE_MODE": mode},
                )
                self.assertFalse(result["ok"])
                self.assertEqual(result["error"]["code"], "INVALID_SHIN_OUTPUT")

    def test_structured_stderr_error_preserves_core_metadata(self) -> None:
        result = self.invoke(
            self.plugin.adapter.discover_project,
            {"root": str(self.root)},
            {"SHIN_FAKE_MODE": "stderr-error"},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["process"]["exit_code"], 2)
        self.assertEqual(result["result"]["schema_version"], "shin.error.v1")
        self.assertEqual(result["error"]["code"], "invalid-option-value")
        self.assertEqual(result["error"]["category"], "usage")
        self.assertEqual(result["error"]["remediation"], "Correct the option and retry.")
        self.assertEqual(result["error"]["exit_code"], 2)
        self.assertEqual(result["process"]["stderr"], "")

    def test_malformed_stderr_errors_fail_closed(self) -> None:
        for mode in (
            "stderr-error-wrong-command",
            "stderr-error-wrong-schema",
            "stderr-error-wrong-tool",
            "stderr-error-ok-string",
            "stderr-duplicate-key",
            "stderr-non-finite",
        ):
            with self.subTest(mode=mode):
                result = self.invoke(
                    self.plugin.adapter.discover_project,
                    {"root": str(self.root)},
                    {"SHIN_FAKE_MODE": mode},
                )
                self.assertFalse(result["ok"])
                self.assertEqual(result["error"]["code"], "INVALID_SHIN_OUTPUT")

    def test_oversized_stderr_fails_before_error_parsing(self) -> None:
        result = self.invoke(
            self.plugin.adapter.discover_project,
            {"root": str(self.root)},
            {"SHIN_FAKE_MODE": "stderr-too-large"},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "SHIN_OUTPUT_TOO_LARGE")
        self.assertEqual(result["error"]["details"]["stream"], "stderr")

    def test_malformed_ready_output_cannot_continue_retrieval(self) -> None:
        result = self.invoke(
            self.plugin.adapter.brief_context,
            {"root": str(self.root), "query": "bounded query"},
            {"SHIN_FAKE_MODE": "ready-malformed"},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "INVALID_SHIN_OUTPUT")
        self.assertEqual(self.call_log.read_text(encoding="utf-8").splitlines(), ["ready"])

    def test_pack_passes_a_bounded_in_memory_request(self) -> None:
        request = {
            "schema_version": "shin.task-request.v1",
            "request_id": "adapter-test",
            "task": {
                "objective": "Load one fixture contract.",
                "scope": [],
                "must": [{"id": "fixture", "query": "fixture contract"}],
                "should": [],
            },
            "budget": {
                "max_evidence_bytes": 12000,
                "max_records": 8,
                "per_query_records": 4,
                "per_document_records": 2,
            },
        }
        result = self.invoke(
            self.plugin.adapter.pack_context,
            {"root": str(self.root), "task_request": request, "jobs": 2},
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"]["task_request"], request)
        self.assertIn("2", result["result"]["argv"])

    def test_pack_rejects_an_unbounded_budget(self) -> None:
        request = {
            "schema_version": "shin.task-request.v1",
            "budget": {
                "max_evidence_bytes": 999999,
                "max_records": 8,
                "per_query_records": 4,
                "per_document_records": 2,
            },
        }
        result = self.invoke(
            self.plugin.adapter.pack_context,
            {"root": str(self.root), "task_request": request},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "TASK_BUDGET_TOO_LARGE")

    def test_marker_traversal_is_rejected(self) -> None:
        result = self.invoke(
            self.plugin.adapter.ready_context,
            {"root": str(self.root), "marker": "../outside.json"},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "MARKER_OUTSIDE_ROOT")

    def test_generated_index_inside_consumer_is_rejected(self) -> None:
        result = self.invoke(
            self.plugin.adapter.ready_context,
            {"root": str(self.root), "out_dir": str(self.root / "generated")},
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "CONSUMER_INDEX_REJECTED")

    def test_marker_route_identifiers_cannot_construct_paths(self) -> None:
        marker = json.loads(self.marker.read_text(encoding="utf-8"))
        marker["index"]["key"] = "../../consumer"
        self.marker.write_text(json.dumps(marker), encoding="utf-8")
        result = self.invoke(
            self.plugin.adapter.ready_context, {"root": str(self.root)}
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "INVALID_ADOPTION_ROUTE")

    def test_published_index_symlink_into_consumer_is_rejected(self) -> None:
        shutil.rmtree(self.out_dir)
        (self.root / "docs" / "kb" / "generated").mkdir(parents=True)
        (self.root / "docs" / "kb" / "generated" / "manifest.json").write_text(
            "{}\n", encoding="utf-8"
        )
        self.out_dir.symlink_to(self.root, target_is_directory=True)
        result = self.invoke(
            self.plugin.adapter.ready_context, {"root": str(self.root)}
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "CONSUMER_INDEX_REJECTED")

    def test_multiple_markers_require_an_explicit_choice(self) -> None:
        second = self.root / "docs" / "shin-context-engine-adoption.json"
        second.write_text(self.marker.read_text(encoding="utf-8"), encoding="utf-8")
        result = self.invoke(
            self.plugin.adapter.ready_context, {"root": str(self.root)}
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "AMBIGUOUS_ADOPTION_MARKER")

    def test_profile_suggestion_requires_an_explicit_contract(self) -> None:
        routing = self.base / "routing.json"
        routing.write_text("{}\n", encoding="utf-8")
        result = self.invoke(
            self.plugin.adapter.suggest_profile,
            {
                "routing_config": str(routing),
                "query": "Example Runtime Connector",
                "from_root": str(self.root),
            },
        )
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"]["command"], "suggest-profile")


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HermesPluginTests)
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if outcome.wasSuccessful() else 1)
