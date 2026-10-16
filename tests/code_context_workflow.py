"""Pilot-only tool boundary. No planner, ranking policy or durable task service."""

from dataclasses import dataclass
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time

import test_semantic_runtime as support
from shin_semantic.artifact import FNV, Rejected, canonical, decode, encode, read, relative, require, sha
from shin_semantic.compact import expand_read

ROOT = Path(__file__).resolve().parents[1]
MAX_CALLS = 12
MAX_ROUNDS = 3
MAX_INSPECTIONS = 3
MAX_RESPONSE_BYTES = 65536
ERROR_RESERVE = 512
TASK_SECONDS = 180
TOOL_SECONDS = 30
TOOL_NAME = "shin_context"
QUOTA_STOPS = {"pilot-call-budget", "pilot-round-budget", "pilot-inspection-budget", "pilot-response-budget",
               "pilot-tool-time-budget", "pilot-native-time-budget"}
CANCELLED_REPLY = b'{"cancelled":true}\n'
SCHEMAS = {"query": "shin.agent-query.v1", "inspect": "shin.agent-inspect.v1",
           "pack": "shin.agent-pack.v1", "read": "shin.agent-read.v1"}
TOOL_SCHEMA = {"name": TOOL_NAME, "description":
    "Read-only project context: query discovers records; inspect lists a relative file; pack collects caller-supplied "
    "requirements; read materializes published source. Inspect also returns bounded one-hop declared relations; "
    "follow their paths only after checking direction, provenance and omission counts. They are not compiler call resolution. "
    "Query/inspect argument is plain text. Pack/read argument "
    "is a JSON string. Once query finds a code file, read it directly with "
    "{schema_version:'shin.source-read-request.v2',snapshot_id:<observed snapshot>,file_paths:[<observed path>]}. "
    "For files omitted by the byte limit, inspect then use {schema_version:'shin.source-read-request.v1',snapshot_id:<observed snapshot>,"
    "record_ids:[<observed source-fact IDs>]}. For pack use {schema_version:'shin.task-request.v1',request_id:<id>,"
    "task:{objective:<question>,scope:[],must:[{id:<id>,query:<focused query>,require_full_coverage:true}],should:[]},"
    "evidence_types:['fact'],budget:{max_evidence_bytes:8192,max_records:8,per_query_records:3,per_document_records:3}}. "
    "Encode those objects as valid JSON, not JavaScript. Choose paths and IDs from observations, never guess them. "
    "Query v2 record_defaults/range_defaults and candidate_defaults apply to every corresponding row; "
    "an omitted evidence_id equals id. Budget fields include this response and preserve an error reserve. "
    "Read v2 numbered_text prefixes are physical line numbers followed by a tab, not source bytes; cite those numbers. "
    "At a quota stop, finish using delivered evidence and state gaps; cancelled queued calls supply no evidence. "
    "No filesystem, shell or other tools are available.",
    "parameters": {"type": "object", "properties": {
        "operation": {"type": "string", "enum": list(SCHEMAS)},
        "argument": {"type": "string"}}, "required": ["operation", "argument"], "additionalProperties": False}}


@dataclass(frozen=True)
class Captured:
    output: bytes
    exit_code: int
    cli_wall_seconds: float


class CliBackend:
    def __init__(self, config, python, scratch):
        self.session = support.runtime.Session(config)
        self.config = self.session.config
        self.python = Path(python).absolute()
        self.python_binary = canonical(str(self.python.resolve(strict=True)))
        self.scratch = canonical(str(scratch))
        require(self.scratch.is_dir() and self.scratch.stat().st_mode & 0o777 == 0o700, "unsafe-pilot-scratch")
        self.python_sha = sha(read(self.python_binary, 64 * 1024 * 1024))

    def invoke(self, operation, argument, timeout):
        self.session.verify_route()
        require(self.python.resolve(strict=True) == self.python_binary and
                sha(read(self.python_binary, 64 * 1024 * 1024)) == self.python_sha, "pilot-interpreter-changed")
        command = [str(self.python), str(ROOT / "tools/shin.py"), "--config", str(self.session.path)]
        request_file = None
        try:
            if operation in ("read", "pack"):
                descriptor, path = tempfile.mkstemp(prefix="request-", suffix=".json", dir=self.scratch)
                request_file = Path(path)
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(encode(argument))
                command += ["--read" if operation == "read" else "--task", path]
            elif operation == "inspect":
                command += ["--inspect", argument]
            else:
                command += [argument]
            started = time.monotonic()
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, start_new_session=True,
                                       env={"PATH": "", "LC_ALL": "C", "PYTHONDONTWRITEBYTECODE": "1"})
            try:
                output, errors = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                raise Rejected("pilot-tool-timeout") from None
            finally:
                if process.poll() is None:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait()
                process.stdout.close()
                process.stderr.close()
            require(not errors and len(output) <= 128 * 1024 + 1, "pilot-cli-transport-rejected")
            self.session.verify_route()
            return Captured(output, process.returncode, time.monotonic() - started)
        finally:
            if request_file is not None:
                request_file.unlink(missing_ok=True)


class Workflow:
    """Serial task ledger; the harness delivers only bytes returned by invoke()."""

    def __init__(self, backend, allowed_paths, *, clock=time.monotonic):
        require(bool(allowed_paths) and all(relative(path) for path in allowed_paths), "invalid-pilot-corpus")
        self.backend, self.allowed_paths, self.clock = backend, frozenset(allowed_paths), clock
        self.started = clock()
        self.lock = threading.Lock()
        self.calls = self.requests = self.backend_calls = self.rounds = self.inspections = self.delivered_bytes = 0
        self.cancelled_calls = 0
        self.tool_seconds = 0.0
        self.snapshot_id = None
        self.stopped = None
        self.closed = False
        self.steps = []
        self.trace = []

    def _failure(self, code, operation, started, captured=None):
        self.stopped = code
        payload = encode({"schema_version": "shin.agent-workflow-error.v1", "ok": False,
                          "error": {"code": code}, "restart_needed": code in (
                              "pilot-snapshot-changed", "pilot-route-changed"), "task_stopped": True,
                          "final_answer_allowed": self.can_finalize()}) + b"\n"
        require(len(payload) + (MAX_CALLS - 1) * len(CANCELLED_REPLY) <= ERROR_RESERVE, "pilot-error-reserve")
        require(self.delivered_bytes + len(payload) <= MAX_RESPONSE_BYTES, "pilot-error-budget")
        return self._deliver(payload, operation, started, captured, False, code)

    def can_finalize(self):
        return self.stopped in QUOTA_STOPS and self.remaining_seconds() > 0

    def cancel_queued(self):
        with self.lock:
            require(not self.closed and self.can_finalize() and self.cancelled_calls < MAX_CALLS - 1,
                    "pilot-task-already-stopped")
            require(self.delivered_bytes + len(CANCELLED_REPLY) <= MAX_RESPONSE_BYTES, "pilot-error-budget")
            self.requests += 1
            self.cancelled_calls += 1
            return self._deliver(CANCELLED_REPLY, "cancelled", self.clock(), None, False, self.stopped)

    def _with_budget(self, result):
        require("budget_remaining" not in result, "pilot-reserved-field")
        result["budget_remaining"] = {
            "calls": MAX_CALLS - self.calls, "discovery_rounds": MAX_ROUNDS - self.rounds,
            "inspections_in_round": MAX_INSPECTIONS - self.inspections,
            "response_bytes": MAX_RESPONSE_BYTES, "reserved_error_bytes": ERROR_RESERVE,
            "tool_milliseconds": max(0, int((TOOL_SECONDS - self.tool_seconds) * 1000)),
            "task_milliseconds": int(self.remaining_seconds() * 1000)}
        maximum_size = len(encode(result)) + 1
        require(self.delivered_bytes + maximum_size <= MAX_RESPONSE_BYTES - ERROR_RESERVE, "pilot-response-budget")
        result["budget_remaining"]["response_bytes"] = MAX_RESPONSE_BYTES - self.delivered_bytes - maximum_size
        raw = encode(result)
        # At most four JSON whitespace bytes keep self-inclusive byte accounting exact.
        return raw + b" " * (maximum_size - len(raw) - 1) + b"\n"

    def _deliver(self, payload, operation, started, captured, evidence, error=None):
        self.delivered_bytes += len(payload)
        self.steps.append({"operation": operation, "round": self.rounds, "evidence_delivered": evidence,
            "delivered_bytes": len(payload), "captured_bytes": len(captured.output) if captured else 0,
            "cli_wall_seconds": captured.cli_wall_seconds if captured else None,
            "operation_wall_seconds": round(self.clock() - started, 6), "error": error})
        self.trace.append(payload.decode())
        return payload

    def _check_paths(self, value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "path":
                    require(child in self.allowed_paths if isinstance(child, str) else False, "pilot-corpus-violation")
                self._check_paths(child)
        elif isinstance(value, list):
            for child in value:
                self._check_paths(child)

    def invoke(self, name, args):
        with self.lock:
            require(self.stopped is None and not self.closed, "pilot-task-already-stopped")
            self.requests += 1
            started = self.clock()
            operation = "invalid"
            captured = None
            if self.calls == MAX_CALLS:
                return self._failure("pilot-call-budget", operation, started)
            self.calls += 1
            try:
                require(name == TOOL_NAME and isinstance(args, dict) and set(args) == {"operation", "argument"},
                        "pilot-tool-not-allowed")
                require(isinstance(args["operation"], str) and args["operation"] in SCHEMAS
                        and isinstance(args["argument"], str)
                        and 0 < len(args["argument"].encode()) <= 128 * 1024, "invalid-pilot-action")
                operation = args["operation"]
                require(self.clock() - self.started < TASK_SECONDS, "pilot-task-deadline")
                argument = args["argument"]
                if operation == "query":
                    require(self.rounds < MAX_ROUNDS, "pilot-round-budget")
                    self.rounds += 1
                    self.inspections = 0
                else:
                    require(self.rounds > 0, "pilot-discovery-required")
                if operation == "inspect":
                    require(argument in self.allowed_paths, "pilot-corpus-violation")
                    require(self.inspections < MAX_INSPECTIONS, "pilot-inspection-budget")
                    self.inspections += 1
                if operation in ("pack", "read"):
                    argument = decode(argument.encode())
                    require(isinstance(argument, dict), "invalid-pilot-action")
                    if operation == "read":
                        require(argument.get("snapshot_id") == self.snapshot_id, "pilot-snapshot-changed")
                remaining = min(TASK_SECONDS - (self.clock() - self.started), TOOL_SECONDS - self.tool_seconds)
                require(remaining > 0, "pilot-tool-time-budget")
                timeout = min(5, remaining) if operation in ("inspect", "read") else remaining
                backend_started = self.clock()
                self.backend_calls += 1
                try:
                    captured = self.backend.invoke(operation, argument, timeout)
                finally:
                    backend_seconds = self.clock() - backend_started
                    self.tool_seconds += backend_seconds
                require(self.clock() - self.started <= TASK_SECONDS, "pilot-task-deadline")
                require(self.tool_seconds <= TOOL_SECONDS, "pilot-tool-time-budget")
                if operation in ("inspect", "read"):
                    require(backend_seconds <= 5, "pilot-native-time-budget")
                result = decode(captured.output)
                require(result.get("schema_version") == SCHEMAS[operation] or
                        (operation == "query" and result.get("schema_version") == "shin.agent-query.v2") or
                        (operation == "read" and result.get("schema_version") == "shin.agent-read.v2"),
                        "pilot-response-mismatch")
                if operation == "read" and result.get("schema_version") == "shin.agent-read.v2":
                    expand_read(result)
                require(captured.exit_code in (0, 1) and type(result.get("ok")) is bool
                        and captured.exit_code == (0 if result["ok"] else 1), "pilot-response-mismatch")
                native = result.get("native")
                # Incomplete Pack remains admitted evidence, unlike a failed native admission.
                require(isinstance(native, dict), "pilot-admission-rejected")
                require(result["ok"] or operation == "pack", "pilot-admission-rejected")
                freshness = native.get("index_freshness", {})
                identity = native.get("snapshot_id")
                require(isinstance(identity, str) and FNV.fullmatch(identity)
                        and freshness.get("snapshot_id") == identity and freshness.get("current") is True
                        and freshness.get("fully_verified") is True, "pilot-currentness-rejected")
                require(self.snapshot_id in (None, identity), "pilot-snapshot-changed")
                self._check_paths(result)
                payload = self._with_budget(result)
                self.snapshot_id = identity
                return self._deliver(payload, operation, started, captured, True)
            except Rejected as error:
                code = str(error)
                if code in ("route-config-changed", "core-binary-changed", "profile-changed", "root-identity-changed"):
                    code = "pilot-route-changed"
                return self._failure(code, operation if operation in SCHEMAS else "invalid", started, captured)
            except Exception:
                return self._failure("pilot-operation-rejected", operation if operation in SCHEMAS else "invalid", started, captured)

    def remaining_seconds(self):
        return max(0.0, TASK_SECONDS - (self.clock() - self.started))

    def close(self):
        with self.lock:
            if self.closed:
                return None
            self.closed = True
            if self.stopped is None and self.remaining_seconds() == 0:
                return self._failure("pilot-task-deadline", "finish", self.clock())
            return None

    def receipt(self):
        return {"schema_version": "shin.agent-workflow-receipt.v1", "snapshot_id": self.snapshot_id,
            "status": "stopped" if self.stopped else "closed" if self.closed else "open", "stop_reason": self.stopped,
            "calls": self.calls, "requests": self.requests, "backend_calls": self.backend_calls,
            "cancelled_calls": self.cancelled_calls,
            "rounds": self.rounds, "delivered_bytes": self.delivered_bytes,
            "tool_wall_seconds": round(self.tool_seconds, 6), "elapsed_seconds": round(self.clock() - self.started, 6),
            "provider_usage": None, "core_phase_timings": None, "model_phase_timings": None,
            "limits": {"calls": MAX_CALLS, "rounds": MAX_ROUNDS, "inspections_per_round": MAX_INSPECTIONS,
                       "response_bytes": MAX_RESPONSE_BYTES, "error_reserve_bytes": ERROR_RESERVE,
                       "tool_seconds": TOOL_SECONDS, "task_seconds": TASK_SECONDS},
            "steps": self.steps, "acceptance": "requires-source-and-answer-review",
            "boundary": "pilot-local-tool-boundary-not-installed-client-policy-or-query-journal"}
