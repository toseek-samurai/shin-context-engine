"""Model-driven R1 fixture phases; the parent owns stand lifetime and final review."""

import hashlib
import json
import stat
import subprocess
import sys

from integration_test import IntegrationFailure, REPOSITORY_ROOT
from retrieval_integration_test import RetrievalIntegrationSuite


SLOT = "/api/v1/projects/workflow-project/task-checkpoints/workflow"
POLICY = "Publication authorization also requires a reviewed target."
write_json = RetrievalIntegrationSuite.replace_private_json
write_text = RetrievalIntegrationSuite.write_fixture_file


def require(condition, message):
    if not condition:
        raise IntegrationFailure(message)


def verify_checkpoint_storage(path):
    entry = path.lstat()
    require(stat.S_ISREG(entry.st_mode) and stat.S_IMODE(entry.st_mode) == 0o600
            and entry.st_nlink == 1, "checkpoint must be private and have no symlink or hard-link alias")


def source_image(home):
    return {str(path.relative_to(home)): hashlib.sha256(path.read_bytes()).hexdigest()
            for directory in ("source", "profiles") for path in sorted((home / directory).rglob("*"))
            if path.is_file()}


def run_full_agent_phase(hermes, home, phase, report, launch, worker, suite):
    before = source_image(home)
    source = home / "source/docs/policy.md"
    initial_policy = source.read_text()
    state = home / "state"
    if phase == "capture":
        write_text(home / "check_policy.py", "from pathlib import Path\nimport sys\n"
                   "source = Path(sys.argv[1])\n"
                   "assert 'manual approval' in source.read_text(), 'policy approval rule missing'\n"
                   "print('<testsuite name=\"policy\" tests=\"1\" failures=\"0\" errors=\"0\" skipped=\"0\">'\n"
                   "      '<testcase name=\"requires_approval\" classname=\"policy\"/></testsuite>')\n")
        binding = json.loads((home / "binding.json").read_text())
        binding["authorized_check_argv"] = [sys.executable, str(home / "check_policy.py"), str(source)]
        write_json(home / "binding.json", binding)
    objectives = {
        "capture": (
            "This is an empty, preconfigured fixture. You are explicitly approved to register this project/workspace, "
            "prepare, inspect and quality-preflight its configured route, and activate it only after passing preflight. "
            "Explicitly refresh knowledge and obtain ready bootstrap. Build/check the separate local publication. "
            "Capture execution inputs, then run exactly authorized_check_argv from binding.json and retain its actual "
            "JUnit output; the check code may be inspected but not changed. Import the actual report twice: receipt "
            "authorized-check with TTL3600, and expiring-check with TTL1, both producer dev-admin, real timestamps "
            "and command fingerprint. Place them into the example draft, link its completed assertion only to "
            "authorized-check. Save state/draft.json, a successful native state/local-checkpoint.json, and the exact "
            "HTTP save request as state/save.json. Save that draft remotely, confirm its exact receipts with admin.curl "
            "and retain that request as state/confirm.json; test an identical confirmation retry. Store the final "
            "checkpoint metadata result (not the outer HTTP envelope) in state/metadata.json. The optional short-TTL "
            "receipt may already be expired; do not pretend otherwise. Source and registry must remain unchanged."
        ),
        "return": (
            "Server has restarted. Resume the saved task, distinguish the confirmed current receipt from the expired "
            "one, and exercise read-only denial with reader.curl and separately with admin.curl plus a different task id. "
            "You are explicitly approved to append exactly one blank line and this line with a trailing newline to "
            "source/docs/policy.md: " + POLICY + " Then request Impact and strict resume, inspect the named changed "
            "source and receipt applicability. Do not build, execute tests, save, confirm, clear or activate in this phase."
        ),
        "offline": (
            "Server and PostgreSQL are stopped and source is changed. First inspect stale local checkpoint/resume "
            "and try one remote resume; do not claim a successful remote assessment or mutation. Then you are "
            "explicitly approved to review the changed source, build/check the existing local publication, obtain "
            "current Brief, and capture a new local checkpoint from a pending draft with no receipts. Use "
            "state/reviewed-draft.json and state/reviewed-checkpoint.json. Resume it and run Observer sample/status. "
            "Do not rerun tests, grant local producer trust, change source/registry or attempt remote writes/recovery."
        ),
    }
    prompt = (
        "You are a fresh Shin acceptance-test agent, not a product implementer. Use only the supplied temporary "
        "fixture and AGENT-WORKFLOW.md, workflow-examples.json, binding.json, observer.json and persisted state/. "
        "Read the delivered guide and examples before acting. Do not read repository/test implementation, global "
        "instructions or other projects. Fixture binding: synthetic host-task/host-thread, project workflow-project, "
        "workspace primary, route workflow-route, slot workflow. admin.curl and reader.curl are operator-provisioned "
        "private credentials: pass them directly to curl --config, NEVER read or print contents, trace HTTP, follow "
        "redirects or change destination. Only loopback URL in binding.json is allowed. Only the absolute Core/Observer "
        "binaries and explicitly authorized check may be executed. Ordinary shell file/JSON tools may prepare requests "
        "below state/. Use existing command approvals without disabling them; no delegation, installation or service "
        "management. No source/registry edits except the exact phase-authorized source edit. Index/state writes are "
        "limited to reviewed fixture paths. This is operator approval for the following phase, not general authority: "
        + objectives[phase] + " Report concise outcomes with exact assessment fields and any missing instruction. "
        "A tool exit code or receipt confirmation is not independent execution attestation."
    )
    probe = launch(hermes, home, phase, report, prompt, reasoning="high", turns=32, budget=300)
    checks = []
    failure = None
    try:
        require(probe["process_exit"] == 0, "agent did not finish within its process budget")
        trace = (REPOSITORY_ROOT / probe["trace"]).read_text()
        for filename in ("dev_admin.token", "dev_reader.token"):
            require(suite.read_private_token(filename) not in trace, "agent transcript contains a credential")
        after = source_image(home)
        if phase == "return":
            require(source.read_text() == initial_policy + "\n" + POLICY + "\n", "agent source edit differs from approval")
            before.pop("source/docs/policy.md")
            after.pop("source/docs/policy.md")
        require(after == before, "agent changed source or registry beyond the approved edit")
        checks.append("source-and-registry-scope-preserved")
        if phase == "capture":
            metadata = json.loads((state / "metadata.json").read_text())
            live = worker.http("GET", SLOT)
            require(metadata["revision"] == live["revision"] and live["revision"] >= 2, "confirmed metadata missing")
            saved = json.loads((state / "save.json").read_text())
            require(len(saved["draft"]["receipts"]) == 2, "two reported receipts were not saved")
            assessment = worker.http("POST", SLOT + "/resume", worker.example("resume"))["assessment"]
            receipts = {row["receipt_id"]: row for row in assessment["receipts"]}
            require(assessment["index_current"] and assessment["continuation_ready"] and
                    receipts["authorized-check"]["passed_current"], "current authenticated continuation not established")
            local = worker.core("resume", "--checkpoint", str(state / "local-checkpoint.json"),
                                *worker.task_args(), "--require-current")
            require(local["index_current"] and not any(row["producer_trusted"] for row in local["receipts"]),
                    "local checkpoint missing or inherited Server producer trust")
            verify_checkpoint_storage(state / "local-checkpoint.json")
            checks.extend(["agent-saved-confirmed-server-checkpoint", "agent-captured-independent-local-checkpoint"])
        elif phase == "return":
            metadata = json.loads((state / "metadata.json").read_text())
            require(worker.http("GET", SLOT)["revision"] == metadata["revision"], "read-only review mutated checkpoint")
            impact = worker.http("POST", SLOT + "/impact", worker.example("impact"))["assessment"]
            stale = worker.http("POST", SLOT + "/resume", worker.example("resume"))["assessment"]
            require(not stale["continuation_ready"] and not stale["index_current"] and
                    not any(row["usable"] for row in stale["receipts"]) and
                    any(row["path"] == "docs/policy.md" for row in impact["reread"]), "agent edit did not yield stale review")
            checks.append("agent-edit-invalidates-continuation-without-mutating-checkpoint")
        else:
            verify_checkpoint_storage(state / "reviewed-checkpoint.json")
            local = worker.core("resume", "--checkpoint", str(state / "reviewed-checkpoint.json"),
                                *worker.task_args(), "--require-current")
            require(local["index_current"] and local["continuation_ready"] and not local["receipts"],
                    "agent did not establish a pending current local checkpoint")
            output = subprocess.run([worker.binding["observer"], "status", "--config", str(home / "observer.json")],
                                     capture_output=True, text=True, timeout=30)
            require(output.returncode == 0 and json.loads(output.stdout)["ok"], "agent local Observer state unavailable")
            checks.extend(["agent-offline-local-continuation-without-fabricated-tests", "agent-local-observer-state-readable"])
    except Exception as error:
        failure = str(error)
    return {"phase": phase, "ok": failure is None, "checks": checks, "failure": failure,
            "behavior_trace_review_required": True}, probe
