#!/usr/bin/env python3
"""Replay delivered R1 requests across fresh processes, not a model-agent proof."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time

from integration_test import IntegrationFailure, IntegrationSuite, REPOSITORY_ROOT, under_build
from retrieval_integration_test import RetrievalIntegrationSuite


DELIVERY = REPOSITORY_ROOT / "integrations/shin-server"
PROJECT = "workflow-project"
ROUTE = "workflow-route"
SLOT = f"/api/v1/projects/{PROJECT}/task-checkpoints/workflow"
ROUTE_URL = f"/api/v1/projects/{PROJECT}/routes/{ROUTE}"
write_json = RetrievalIntegrationSuite.replace_private_json
write_text = RetrievalIntegrationSuite.write_fixture_file


def require(condition, message):
    if not condition:
        raise IntegrationFailure(message)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def build_path(value):
    path = Path(value).resolve()
    if not under_build(path):
        raise argparse.ArgumentTypeError("path must stay in the native build directory")
    return path


def clean_environment():
    return {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}


def delivery_inputs(args):
    prefix = args.installed_prefix
    if prefix is None:
        return DELIVERY, REPOSITORY_ROOT / "profiles/generic-docs.json"
    prefix = prefix.resolve(strict=True)
    for name in ("core", "server", "observer"):
        tool = {"core": "shin-context", "server": "shin-server", "observer": "shin-observer"}[name]
        expected = prefix / "bin" / tool
        require(expected.is_file() and not expected.is_symlink() and
                getattr(args, name).resolve(strict=True) == expected,
                f"installed {name} binary must belong to the selected prefix")
    delivery = prefix / "share/shin-candidate/agent"
    profile = prefix / "share/shin-context/profiles/generic-docs.json"
    for path in (delivery / "AGENT-WORKFLOW.md", delivery / "workflow-examples.json", profile):
        require(path.is_file() and path.resolve().is_relative_to(prefix),
                "installed delivery input is missing or escapes the selected prefix")
    return delivery, profile


class WorkflowProcess:
    """Test-only transport using the shipped JSON examples and private curl file."""

    def __init__(self, home: Path):
        self.home = home
        self.binding = read_json(home / "binding.json")
        self.examples = read_json(home / "workflow-examples.json")
        self.state = home / "state"
        self.checks = []

    def passed(self, name):
        self.checks.append(name)

    def example(self, name, **values):
        return dict(copy.deepcopy(self.examples[name]), **values)

    def http(self, method, path, body=None, *, role="admin", expected=(200,), key=False,
             unavailable=False):
        response_path = self.state / "http-response.json"
        response_path.unlink(missing_ok=True)
        command = ["/usr/bin/curl", "--disable", "--config", str(self.home / f"{role}.curl"),
                   "--noproxy", "*", "--silent", "--show-error", "--request", method,
                   "--output", str(response_path), "--write-out", "%{http_code}"]
        if body is not None:
            write_json(self.state / "http-request.json", body)
            command += ["--data-binary", "@" + str(self.state / "http-request.json")]
        if key:
            command += ["--header", "Idempotency-Key: " + secrets.token_hex(16)]
        command += [self.binding["server_url"] + path]
        result = subprocess.run(command, capture_output=True, text=True, timeout=25)
        if unavailable:
            require(result.returncode == 7 and result.stdout == "000",
                    "offline HTTP unexpectedly connected or failed for a different reason")
            return None
        require(result.returncode == 0, f"curl failed with status {result.returncode}")
        envelope = read_json(response_path)
        status = int(result.stdout)
        require(status in expected, f"{method} {path}: HTTP {status}, "
                f"code={envelope.get('error', {}).get('code')}")
        require(envelope.get("ok") == (status < 400), "HTTP and envelope success disagree")
        # Workspace registration intentionally returns its reviewed root; context projections do not.
        rendered = json.dumps(envelope)
        if not path.endswith("/workspaces"):
            require(self.binding["root"] not in rendered and str(self.home) not in rendered,
                    f"{path}: context response disclosed a private host coordinate")
        return envelope["result"] if status < 400 else envelope["error"]

    def core(self, *args, expected=(0,)):
        b = self.binding
        command = [b["core"], "--profile", b["profile"], "--profile-dir", b["registry"],
                   "--root", b["root"], "--out-dir", b["local_out"], *args]
        result = subprocess.run(command, capture_output=True, text=True, timeout=45,
                                env=dict(clean_environment(), PATH=""))
        payload = json.loads(result.stdout or result.stderr)
        require(result.returncode in expected, f"Core {args[0]} failed: "
                f"{payload.get('error', {}).get('code', result.returncode)}")
        return payload

    def task_args(self):
        return ["--task-id", self.binding["task_id"], "--thread-id", self.binding["thread_id"]]

    def mutation(self, name, revision, **values):
        return self.example(name, expected_revision=revision, request_id=secrets.token_hex(16),
                            request_deadline=int(time.time()) + 120, **values)

    def attach(self):
        self.http("POST", "/api/v1/projects", self.example("project"), expected=(201,), key=True)
        self.http("POST", f"/api/v1/projects/{PROJECT}/workspaces",
                  self.example("workspace", root=self.binding["root"]), expected=(201,), key=True)
        denied = self.http("POST", ROUTE_URL + "/activation", self.example("activate"),
                           role="reader", expected=(403,))
        require(denied["code"] == "forbidden", "reader activation denial changed")
        self.passed("attach-and-reader-denial")
        self.prepare_and_activate()
        context = self.refresh_context()
        require(not context["checkpoint_restored"], "bootstrap unexpectedly restored a checkpoint")
        self.passed("bootstrap-ready-without-checkpoint-restoration")

    def prepare_and_activate(self):
        active = self.http("GET", ROUTE_URL + "/activation")
        job = self.http("POST", ROUTE_URL + "/preparations",
                        self.example("prepare", expected_snapshot_id=active["snapshot_id"]),
                        expected=(202,), key=True)["job"]
        deadline = time.monotonic() + 30
        while job["state"] in {"queued", "running"} and time.monotonic() < deadline:
            time.sleep(0.1)
            job = self.http("GET", f"/api/v1/preparations/{job['id']}")
        require(job["state"] == "prepared", f"preparation did not succeed: {job['state']}")
        self.http("GET", f"/api/v1/preparations/{job['id']}/inspection")
        preflight = self.http("GET", f"/api/v1/preparations/{job['id']}/preflight")
        require(preflight["preflight_passed"] and preflight["quality_passed"], "preflight failed")
        self.http("POST", ROUTE_URL + "/activation",
                  self.example("activate", preparation_id=job["id"], expected_revision=active["revision"]))
        self.passed("prepare-inspect-preflight-explicit-activation")

    def refresh_context(self):
        self.http("POST", "/api/v1/knowledge-maintenance/refresh", self.example("refresh"), expected=(202,))
        deadline = time.monotonic() + 30
        while True:
            context = self.http("POST", "/api/v1/session-bootstrap", self.example("bootstrap"))
            if context["ready"] or time.monotonic() >= deadline:
                break
            time.sleep(0.1)
        require(context["ready"] and context["context_ready"] and context["health_ready"],
                "bootstrap did not obtain independent ready evidence")
        require("docs/policy.md" in json.dumps(context), "bootstrap omitted the source to verify")
        return context

    def capture(self):
        self.core("build")
        self.core("check")
        self.core("doctor", "--strict")
        inputs_request = self.state / "inputs-request.json"
        write_json(inputs_request, self.example("executionInputs"))
        inputs = self.core("receipt-scope", "--input", str(inputs_request), *self.task_args(), "--require-current")
        inputs_path = self.state / "inputs.json"
        write_json(inputs_path, inputs)
        started = int(time.time())
        # This is a real, separately authorized fixture check; Shin only imports its report.
        command = [sys.executable, "-c", "from pathlib import Path; import sys; "
                   "assert 'manual approval' in Path(sys.argv[1]).read_text(); "
                   "print('<testsuite name=\"policy\" tests=\"1\" failures=\"0\" errors=\"0\" skipped=\"0\">'"
                   " '<testcase name=\"requires_approval\" classname=\"policy\"/></testsuite>')",
                   str(Path(self.binding["root"]) / "docs/policy.md")]
        run = subprocess.run(command, capture_output=True, text=True, timeout=10)
        require(run.returncode == 0, "authorized external policy test failed")
        report_path = self.state / "report.xml"
        write_text(report_path, run.stdout)
        receipts = []
        for name, ttl in (("authorized-check", 3600), ("expiring-check", 1)):
            request = self.example("reportImport", receipt_id=name, producer="dev-admin",
                                   report_path=str(report_path), inputs_path=str(inputs_path),
                                   started_at=started, observed_at=int(time.time()), ttl_seconds=ttl,
                                   command_fingerprint="sha256:" + hashlib.sha256(
                                       json.dumps(command, ensure_ascii=False, separators=(",", ":")).encode("utf-8")).hexdigest())
            import_path = self.state / "report-import.json"
            write_json(import_path, request)
            bundle = self.core("receipt-import", "--input", str(import_path),
                               *self.task_args(), "--require-current")
            receipts.extend(bundle["receipts"])
        draft = self.example("draft", receipts=receipts)
        draft["state"]["assertions"][0].update(kind="completed", receipt_ids=["authorized-check"])
        draft_path = self.state / "draft.json"
        write_json(draft_path, draft)
        checkpoint = self.core("checkpoint", "--input", str(draft_path), *self.task_args(), "--require-current")
        write_json(self.state / "local-checkpoint.json", checkpoint)
        self.passed("real-check-import-and-local-checkpoint")
        absent = self.http("GET", SLOT, expected=(404,))
        require(absent["code"] == "checkpoint-not-found", "new slot is not absent")
        save = self.mutation("save", 0, draft=draft)
        write_json(self.state / "save.json", save)
        saved = self.http("POST", SLOT, save, expected=(200, 201))
        assessment = self.http("POST", SLOT + "/resume", self.example("resume"))["assessment"]
        require(not any(row["producer_trusted"] for row in assessment["receipts"]),
                "saving producer names granted trust")
        confirm = self.mutation("confirmReceipts", saved["revision"], receipts=receipts)
        write_json(self.state / "confirm.json", confirm)
        confirmed = self.http("POST", SLOT + "/receipts", confirm)
        replay = self.http("POST", SLOT + "/receipts", confirm)
        require(replay["replayed"] and replay["revision"] == confirmed["revision"], "retry changed receipt state")
        write_json(self.state / "metadata.json", confirmed)
        self.passed("untrusted-save-exact-confirmation-and-retry")

    def return_and_review(self):
        metadata = read_json(self.state / "metadata.json")
        require(self.http("GET", SLOT)["revision"] == metadata["revision"], "restart lost checkpoint revision")
        self.http("GET", SLOT, role="reader", expected=(404,))
        self.http("POST", SLOT + "/resume", self.example("resume", task_id="another-task"), expected=(404,))
        self.passed("restart-private-owner-and-task-binding")
        time.sleep(2)
        assessment = self.http("POST", SLOT + "/resume", self.example("resume"))["assessment"]
        rows = {row["receipt_id"]: row for row in assessment["receipts"]}
        require(assessment["index_current"] and assessment["continuation_ready"], "current resume failed")
        require(rows["authorized-check"]["passed_current"], "confirmed current check was not usable")
        require(rows["expiring-check"]["age"] == "expired" and not rows["expiring-check"]["usable"],
                "confirmation bypassed receipt expiry")
        self.passed("current-resume-and-independent-receipt-expiry")
        source = Path(self.binding["root"]) / "docs/policy.md"
        write_text(source, source.read_text() + "\nPublication authorization also requires a reviewed target.\n")
        impact = self.http("POST", SLOT + "/impact", self.example("impact"))["assessment"]
        require(not impact["index_current"] and any(row["path"] == "docs/policy.md" for row in impact["reread"]),
                "Impact omitted changed source")
        stale = self.http("POST", SLOT + "/resume", self.example("resume"))["assessment"]
        require(not stale["ok"] and not stale["continuation_ready"] and stale["continuation_pack"] is None,
                "strict resume admitted stale evidence")
        require(not any(row["usable"] for row in stale["receipts"]), "old receipts survived input drift")
        require(self.http("GET", SLOT)["revision"] == metadata["revision"], "read-only assessment changed revision")
        self.passed("source-edit-impact-and-stale-continuation-denial")

    def offline(self):
        self.http("POST", SLOT, read_json(self.state / "save.json"), unavailable=True)
        self.passed("offline-mutation-not-acknowledged")
        local = self.core("resume", "--checkpoint", str(self.state / "local-checkpoint.json"),
                          *self.task_args(), "--require-current", expected=(1,))
        require(not local["continuation_ready"], "offline resume hid stale source")
        self.passed("offline-stale-local-checkpoint-remains-reviewable")
        self.core("build")
        self.core("check")
        self.core("brief", "publication authorization", "--projection", "compact", "--require-current")
        # Explicit local review creates a pending draft, never a fabricated test rerun.
        draft_path = self.state / "reviewed-draft.json"
        write_json(draft_path, self.example("draft"))
        checkpoint = self.core("checkpoint", "--input", str(draft_path), *self.task_args(), "--require-current")
        write_json(self.state / "reviewed-checkpoint.json", checkpoint)
        resumed = self.core("resume", "--checkpoint", str(self.state / "reviewed-checkpoint.json"),
                            *self.task_args(), "--require-current")
        require(resumed["index_current"] and resumed["continuation_ready"] and not resumed["receipts"],
                "explicit local review did not recover current context or fabricated execution")
        self.passed("offline-explicit-local-build-and-resume-without-forged-receipts")
        for command in ("sample", "status"):
            run = subprocess.run([self.binding["observer"], command, "--config", str(self.home / "observer.json")],
                                 capture_output=True, text=True, timeout=30, env=dict(clean_environment(), PATH=""))
            require(run.returncode == 0 and json.loads(run.stdout)["ok"], f"offline Observer {command} failed")
        self.passed("offline-observer-sample-and-status-with-empty-path")


def prepare_fixture(home, core, observer, suite, delivery, profile_path):
    for name in ("source", "profiles", "local-out", "state", "managed", "observer-state"):
        (home / name).mkdir(mode=0o700)
    root = home / "source"
    probe = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True, timeout=10, env=clean_environment())
    require(probe.returncode == 128 and "not a git repository" in probe.stderr.lower(),
            "fixture inherited Git ancestry or Git probe failed unexpectedly")
    write_text(root / "README.md", "# Neutral Workflow\n\nPublication authorization is defined in docs/policy.md.\n")
    write_text(root / "docs/policy.md", "# Publication Authorization\n\nPublication authorization requires manual approval.\n")
    for name in ("AGENT-WORKFLOW.md", "workflow-examples.json"):
        write_text(home / name, (delivery / name).read_text())
    examples = read_json(home / "workflow-examples.json")
    profile = read_json(profile_path)
    profile["evalCases"] = [{"id": "policy", "query": "publication authorization", "mustPaths": ["docs/policy.md"],
                             "maxCandidateRows": -1, "limit": 6, "perDocLimit": 2}]
    profile["packEvalCases"] = [{"id": "policy-pack", "request": examples["draft"]["task_request"],
                                 "mustPaths": ["docs/policy.md"], "expectedComplete": True,
                                 "maxEvidenceBytes": 8192, "maxRecords": 6, "maxCompactBytes": 16000,
                                 "requireSerialParallelIdentity": True}]
    write_json(home / "profiles/generic-docs.json", profile)
    binding = {"core": str(core), "observer": str(observer), "root": str(root), "profile": "generic-docs",
               "registry": str(home / "profiles"), "local_out": str(home / "local-out"),
               "server_url": "http://127.0.0.1:19125", "task_id": "host-task", "thread_id": "host-thread"}
    write_json(home / "binding.json", binding)
    write_json(home / "observer.json", {"schema_version": "shin.observer-config.v2",
               "observer_id": "workflow-observer", "source_host_id": "local-dev", "project_id": PROJECT,
               "workspace_id": "primary", "root": str(root), "profile": "generic-docs",
               "profile_dir": binding["registry"], "out_dir": binding["local_out"],
               "state_dir": str(home / "observer-state"), "poll_interval_ms": 100,
               "max_snapshot_bytes": 1048576, "max_file_bytes": 262144,
               "outbox": {"enabled": False, "max_entries": 64, "max_bytes": 131072}})
    for role, filename in (("admin", "dev_admin.token"), ("reader", "dev_reader.token")):
        token = suite.read_private_token(filename)
        write_text(home / f"{role}.curl", f'header = "Authorization: Bearer {token}"\n'
                   f'header = "X-Shin-Server-ID: {suite.read_runtime_server_id()}"\n'
                   'header = "Content-Type: application/json"\nconnect-timeout = 2\nmax-time = 20\n')


def run_agent_probe(hermes, home, phase, report):
    """A bounded read-only acceptance probe, never delegated product implementation."""
    objectives = {
        "return": "The task was saved and Server restarted. Determine whether current context can be resumed, "
                  "which receipts are usable, and why. Check the same slot with reader.curl and a different "
                  "task id without mutating it. An expiring receipt was deliberately given TTL 1s; wait 2s "
                  "before evaluating. Do not edit source, build, execute tests, save, confirm, clear or activate.",
        "offline": "Server and PostgreSQL are stopped and source has changed. Inspect the saved local "
                   "checkpoint and current source using Core, and sample/status using Observer. Try one "
                   "remote resume to distinguish unavailability from an acknowledged operation. Do not "
                   "build, edit source, execute tests, save, confirm, clear, activate or repair anything.",
    }
    prompt = (
        "You are a fresh acceptance-test agent, not a coding agent. Work only in the supplied temporary "
        "fixture. Read AGENT-WORKFLOW.md, workflow-examples.json, binding.json and, as needed, state/ "
        "metadata.json and local-checkpoint.json. Do not read any repository/test implementation or "
        "global instructions. The fixture uses synthetic host-task/host-thread, project workflow-project, "
        "route workflow-route, slot workflow. state is the private state directory; observer.json is the "
        "private Observer configuration. admin.curl and reader.curl are operator-prepared credentials: "
        "pass them directly to curl --config, NEVER read/print their contents, never trace curl or enable "
        "verbose HTTP. Absolute binaries in binding.json are authorized only for the relevant read-only "
        "commands. Requests/reports may be written only below state/. Do not access other projects, "
        "network destinations, configuration or credentials. No delegation or tool installation. "
        + objectives[phase] + " Return a concise evidence-backed account, including exact assessment "
        "fields and HTTP/transport outcomes, and any missing instruction. Do not claim a new passing "
        "test execution or independent receipt attestation."
    )
    return execute_agent(hermes, home, phase, report, prompt)


def execute_agent(hermes, home, phase, report, prompt, *, reasoning="medium", turns=16, budget=180):
    prompt_path = home / f"{phase}-prompt.txt"
    write_text(prompt_path, prompt)
    trace = report.parent / (report.stem + f"-{phase}-agent.jsonl")
    error_log = trace.with_suffix(".stderr")
    command = [str(hermes), "chat", "--safe-mode", "--oneshot", "--format", "stream-json",
               "--provider", "openai-codex", "--model", "gpt-6-sol", "--reasoning", reasoning,
               "--toolsets", "terminal", "--max-turns", str(turns), "--run-budget", str(budget),
               "--in", str(home), "--query-file", str(prompt_path)]
    started = time.monotonic()
    with trace.open("x") as output, error_log.open("x") as errors:
        process = subprocess.Popen(command, cwd=home, stdout=output, stderr=errors,
                                   stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            code = process.wait(timeout=budget + 30)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            code = 124
    return {"phase": phase, "process_exit": code, "duration_seconds": round(time.monotonic() - started, 2),
            "trace": str(trace.relative_to(REPOSITORY_ROOT)), "model": "gpt-6-sol", "reasoning": reasoning,
            "review_required": True, "boundary": "Process result only; inspect behavior and independent oracle."}


def run_suite(args, suite_factory=IntegrationSuite, candidate_trial=None):
    model_phases = getattr(args, "agent_phases", None) or ("capture", "return", "offline")
    delivery, profile_path = delivery_inputs(args)
    delivered_paths = {"entrypoint": delivery / "AGENT-WORKFLOW.md",
                       "examples": delivery / "workflow-examples.json", "profile": profile_path}
    delivered_hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for name, path in delivered_paths.items()}
    suite = suite_factory(args.server, args.state_root, args.report)
    originals = {}
    phases = []
    backup_restore = None
    binary_rollback = None
    agent_probes = []
    artifacts = {}
    failure = None
    outside_git = False
    postgres_started = False
    os.umask(0o077)
    with tempfile.TemporaryDirectory(prefix="shin-headless-workflow-") as temporary:
        home = Path(temporary).resolve()
        try:
            suite.check_version()
            suite.dev_command("init")
            suite.require_http_port_free()
            suite.dev_command("up")
            postgres_started = True
            suite.wait_for_postgres()
            prepare_fixture(home, args.core, args.observer, suite, delivery, profile_path)
            outside_git = True
            artifact_paths = {"core": args.core, "server": args.server, "observer": args.observer,
                              "entrypoint": home / "AGENT-WORKFLOW.md", "examples": home / "workflow-examples.json"}
            artifacts = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in artifact_paths.items()}
            for name in ("runtime", "migrator"):
                path = args.state_root / "config" / f"{name}.json"
                originals[path] = path.read_bytes()
                config = json.loads(originals[path])
                require(config["schema_version"] == "shin.server-config.v10", "unexpected stand configuration")
                config["allowed_roots"] = [str(home / "source")]
                config["retrieval"]["routes"] = [{"id": ROUTE, "project_id": PROJECT, "workspace_id": "primary",
                    "root": str(home / "source"), "profile": "generic-docs", "profile_dir": str(home / "profiles")}]
                config["preparations"] = {"enabled": True, "state_dir": str(home / "managed"),
                                          "max_retained_jobs": 8, "lease_seconds": 3}
                config["knowledge"] = {"enabled": True, "principal_id": "dev-admin", "route_ids": [ROUTE],
                                       "interval_seconds": 3600, "max_age_seconds": 7200, "prepare_on_drift": False}
                for principal in config["principals"]:
                    principal["receipt_producer"] = principal["id"] == "dev-admin"
                    if principal["id"] == "dev-reader":
                        principal["project_ids"] = [PROJECT]
                write_json(path, config)
            suite.reset_schema()
            suite.migrate()
            suite.start_server()
            for phase in ("capture", "return", "offline"):
                if phase == "return":
                    suite.stop_server(expect_clean=True)
                    suite.start_server()
                if phase == "offline":
                    suite.stop_server(expect_clean=True)
                    suite.compose("stop", "postgres")
                    running = suite.compose("ps", "--status", "running", "--services").stdout.splitlines()
                    require("postgres" not in running, "PostgreSQL remained running for offline proof")
                    suite.require_http_port_free()
                if args.agent_workflow and phase in model_phases:
                    from headless_agent_probe import run_full_agent_phase
                    result, probe = run_full_agent_phase(args.hermes, home, phase, args.report,
                                                          execute_agent, WorkflowProcess(home), suite)
                    agent_probes.append(probe)
                    phases.append(result)
                    require(result["ok"], f"{phase}: {result.get('failure')}")
                    continue
                if args.hermes and not args.agent_workflow and phase in {"return", "offline"}:
                    agent_probes.append(run_agent_probe(args.hermes, home, phase, args.report))
                process = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker", phase,
                                          "--home", str(home)], capture_output=True, text=True, timeout=120,
                                         env=clean_environment())
                result = read_json(home / f"{phase}-result.json")
                phases.append(result)
                require(process.returncode == 0 and result["ok"], f"{phase}: {result.get('failure')}")
                if phase == "capture" and args.rollback_server:
                    binary_rollback = run_binary_rollback(suite, WorkflowProcess(home), args.rollback_server)
                if phase == "capture" and args.backup_restore:
                    from backup_restore_checks import run_backup_restore
                    backup_restore = run_backup_restore(suite, WorkflowProcess(home), candidate_trial=candidate_trial)
                    require(backup_restore["ok"], f"backup-restore: {backup_restore['failure']}")
            require(all(hashlib.sha256(path.read_bytes()).hexdigest() == artifacts[name]
                        for name, path in artifact_paths.items()), "binary or delivered guidance changed during the run")
            require(all(hashlib.sha256(path.read_bytes()).hexdigest() == delivered_hashes[name]
                        for name, path in delivered_paths.items()), "delivery inputs changed during the run")
        except Exception as error:
            failure = str(error)
        finally:
            try:
                suite.stop_server(expect_clean=True)
                if postgres_started:
                    suite.compose("stop", "postgres")
            except Exception as error:
                failure = failure or str(error)
            for path, content in originals.items():
                # Restore the operator-owned configuration byte-for-byte, including formatting.
                write_text(path, content.decode("utf-8"))
        report = {"schema_version": "shin.headless-workflow-report.v1", "ok": failure is None,
                  "proof_kind": ("mixed-scripted-model-fixture-with-independent-oracle" if args.agent_workflow and len(model_phases) < 3
                                 else "model-driven-fixture-with-independent-oracle" if args.agent_workflow
                                 else "scripted-fresh-process-replay"), "model_agent_proof": False,
                  "outside_git": outside_git, "phases": phases, "failure": failure,
                  "agent_probes": agent_probes,
                  "delivery_source": "installed-prefix" if args.installed_prefix else "source-checkout",
                  "delivered_input_sha256": delivered_hashes,
                  "artifacts": artifacts}
        if args.backup_restore:
            report["backup_restore"] = backup_restore
        if args.rollback_server:
            report["binary_rollback"] = binary_rollback
        write_json(args.report, report)
        print(json.dumps(report))
        return int(failure is not None)


def run_binary_rollback(suite, worker, previous):
    """Exercise read compatibility on the disposable stand, never down-migrate."""
    candidate = suite.binary
    candidate_version = suite.expected_server_version
    identities = {}
    for name, path in (("candidate", candidate), ("previous", previous)):
        raw = suite.run_process([str(path), "--version"], "rollback identity").stdout
        value = json.loads(raw)
        require(value.get("ok") is True and value.get("tool") == "shin-server",
                "rollback binary identity is invalid")
        identities[name] = {"version": value["version"],
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                            "core_version": value["result"]["core_version"],
                            "contracts": value["result"]["contracts"]}
    require(identities["candidate"]["contracts"] == identities["previous"]["contracts"] and
            identities["candidate"]["core_version"] == identities["previous"]["core_version"],
            "rollback control must have matching protocol and Core identities")
    require(identities["candidate"]["sha256"] != identities["previous"]["sha256"],
            "rollback control must differ from the candidate")
    config = suite.state_root / "config/runtime.json"
    original_config = config.read_bytes()
    metadata = worker.http("GET", SLOT)
    request = {"query": "publication authorization", "require_current": True}
    before = worker.http("POST", ROUTE_URL + "/brief", request)
    checks = []
    try:
        suite.stop_server(expect_clean=True)
        suite.require_http_port_free()
        suite.run_process([str(previous), "check-config", "--config", str(config)],
                          "rollback configuration admission")
        for name, path in (("previous", previous), ("candidate", candidate)):
            suite.binary = path
            suite.expected_server_version = identities[name]["version"]
            # The dev37 control predates the Server-owned umask correction.
            suite.start_server(creation_umask=0o077 if name == "previous" else 0)
            require(worker.http("GET", SLOT) == metadata, "rollback changed checkpoint metadata")
            denied = worker.http("POST", ROUTE_URL + "/brief", request, expected=(409,))
            require(denied["code"] == "active-generation-route-changed",
                    "version transition silently reused another version's activation")
            worker.prepare_and_activate()
            after = worker.http("POST", ROUTE_URL + "/brief", request)
            require(all(after[key] == before[key] for key in ("snapshot_id", "evidence")) and
                    after["preparation_id"] != before["preparation_id"],
                    "rollback changed current retrieval identity or evidence")
            before = after
            require(config.read_bytes() == original_config, "rollback changed runtime configuration")
            require(worker.http("GET", SLOT) == metadata, "reprepare changed checkpoint metadata")
            checks.append(name + "-startup-version-fence-explicit-reprepare-and-preserved-checkpoint")
            if name == "previous":
                suite.stop_server(expect_clean=True)
                suite.require_http_port_free()
        for name, path in (("candidate", candidate), ("previous", previous)):
            require(hashlib.sha256(path.read_bytes()).hexdigest() == identities[name]["sha256"],
                    "rollback binary changed during the run")
    finally:
        # The outer suite owns final process cleanup even after a failed restart.
        suite.binary = candidate
        suite.expected_server_version = candidate_version
    return {"ok": True, "identities": identities, "checks": checks,
            "previous_launcher_umask": "0077", "candidate_launcher_umask": "0000",
            "boundary": "Disposable schema019 fixture and read-compatible binaries only; no down-migration, "
                        "old-schema restore or approval to deploy a superseded binary with known defects."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", choices=("capture", "return", "offline"))
    parser.add_argument("--home", type=Path)
    parser.add_argument("--core", type=build_path)
    parser.add_argument("--server", type=build_path)
    parser.add_argument("--observer", type=build_path)
    parser.add_argument("--state-root", type=build_path)
    parser.add_argument("--report", type=build_path)
    parser.add_argument("--installed-prefix", type=build_path,
                        help="require binaries, agent guidance and generic profile from this staged prefix")
    parser.add_argument("--rollback-server", type=build_path,
                        help="explicit prior compatible binary for disposable-stand rollback and return")
    parser.add_argument("--hermes", type=lambda value: Path(value).resolve(),
                        help="optional bounded returning-agent probes; traces require independent review")
    parser.add_argument("--agent-workflow", action="store_true",
                        help="replace scripted fixture actions with fresh Hermes sessions and verify their state")
    parser.add_argument("--agent-phases", nargs="+", choices=("capture", "return", "offline"),
                        help="with --agent-workflow, run only these phases with a model; others stay scripted")
    parser.add_argument("--backup-restore", action="store_true",
                        help="also restore the captured fixture into a clean isolated database and artifact directory")
    args = parser.parse_args()
    if args.worker:
        worker = WorkflowProcess(args.home)
        failure = None
        try:
            if args.worker == "capture":
                worker.attach()
                worker.capture()
            elif args.worker == "return":
                worker.return_and_review()
            else:
                worker.offline()
        except Exception as error:
            failure = str(error)
        write_json(args.home / f"{args.worker}-result.json", {"phase": args.worker, "ok": failure is None,
                   "checks": worker.checks, "failure": failure})
        return int(failure is not None)
    if not all((args.core, args.server, args.observer, args.state_root, args.report)):
        parser.error("provide --core, --server, --observer, --state-root and --report")
    if args.agent_workflow and not args.hermes:
        parser.error("--agent-workflow requires --hermes")
    if args.agent_phases and (not args.agent_workflow or len(set(args.agent_phases)) != len(args.agent_phases)):
        parser.error("--agent-phases requires --agent-workflow and distinct phase names")
    if args.backup_restore and args.agent_workflow:
        parser.error("--backup-restore uses scripted fixture actions, not --agent-workflow")
    if args.rollback_server and args.agent_workflow:
        parser.error("--rollback-server uses scripted fixture actions, not --agent-workflow")
    if args.report.exists():
        parser.error("--report must not overwrite an earlier run")
    for binary in (args.core, args.server, args.observer, *([args.rollback_server] if args.rollback_server else [])):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error("binary must be an executable file")
    try:
        delivery_inputs(args)
    except (OSError, IntegrationFailure) as error:
        parser.error(str(error))
    return run_suite(args)


if __name__ == "__main__":
    sys.exit(main())
