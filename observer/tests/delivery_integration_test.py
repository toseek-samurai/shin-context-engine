#!/usr/bin/env python3
"""Black-box fault harness for the native Shin Observer delivery CLI.

This harness owns a loopback-only fake protocol server.  It establishes only
transport and persistence observations; it deliberately does not claim that a
real Server database committed a submission.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Iterator

from integration_test import CORE_VERSION, RESPONSE_SCHEMA, Fixture, IntegrationFailure, assert_observer_envelope


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
OBSERVER_VERSION = (REPOSITORY_ROOT / "observer" / "VERSION").read_text(encoding="utf-8").strip()
OBSERVER_STABILITY = "development" if "-dev." in OBSERVER_VERSION or "-rc." in OBSERVER_VERSION else "stable"
SERVER_VERSION = (REPOSITORY_ROOT / "server" / "VERSION").read_text(encoding="utf-8").strip()
STATE_SCHEMA = "shin.observer-state.v3"
OUTBOX_SCHEMA = "shin.observer-outbox.v2"
DELIVERY_CONFIG_SCHEMA = "shin.observer-delivery-config.v1"
SERVER_RESPONSE_SCHEMA = "shin.server-response.v1"
MAX_RESPONSE_BYTES = 16 * 1024
TOKEN = "delivery-fixture-token_abcdefghijklmnopqrstuvwxyz0123456789"
SERVER_ID = "a" * 32
OTHER_SERVER_ID = "b" * 32


def compact_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def write_private(path: Path, content: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(content)


def write_private_json(path: Path, value: dict[str, Any]) -> None:
    write_private(path, compact_json(value) + b"\n")


def state_from_result(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise IntegrationFailure("Observer response result is not an object")
    state = value.get("state", value)
    if not isinstance(state, dict) or state.get("schema_version") != STATE_SCHEMA:
        raise IntegrationFailure("Observer did not return shin.observer-state.v3")
    return state


def require_private_regular(path: Path, label: str) -> None:
    entry = path.lstat()
    if not stat.S_ISREG(entry.st_mode) or stat.S_ISLNK(entry.st_mode) or stat.S_IMODE(entry.st_mode) != 0o600:
        raise IntegrationFailure(f"{label} is not an owner-only regular file")


@dataclass(frozen=True)
class Request:
    method: str
    path: str
    headers: dict[str, str]
    server_id_headers: tuple[str, ...]
    body: bytes


@dataclass(frozen=True)
class Reply:
    status: int
    body: bytes = b""
    headers: tuple[tuple[str, str], ...] = ()
    drop_after_commit: bool = False
    stall_seconds: float = 0.0


class ControlledProtocolServer:
    """A single-purpose loopback HTTP peer with explicit per-request replies."""

    def __init__(self, *, server_id: str = SERVER_ID) -> None:
        self.server_id = server_id
        self.identity_replies: list[Reply] = []
        self.submission_replies: list[Reply] = []
        self.reconciliation_replies: list[Reply] = []
        self.requests: list[Request] = []
        self.committed: dict[str, dict[str, Any]] = {}
        self._lock = threading.Lock()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, format: str, *args: Any) -> None:
                return

            def read_request(self) -> Request:
                raw_length = self.headers.get("Content-Length", "0")
                try:
                    length = int(raw_length)
                except ValueError as error:
                    raise IntegrationFailure("fake peer received invalid Content-Length") from error
                if length < 0 or length > MAX_RESPONSE_BYTES:
                    raise IntegrationFailure("Observer sent an oversized request")
                body = self.rfile.read(length)
                server_id_headers = tuple(self.headers.get_all("X-Shin-Server-ID") or ())
                return Request(self.command, self.path, {key.lower(): value for key, value in self.headers.items()}, server_id_headers, body)

            def send_reply(self, reply: Reply) -> None:
                if reply.stall_seconds:
                    time.sleep(reply.stall_seconds)
                self.send_response(reply.status)
                for key, value in reply.headers:
                    self.send_header(key, value)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(reply.body)))
                self.send_header("Connection", "close")
                self.end_headers()
                if reply.body:
                    try:
                        self.wfile.write(reply.body)
                    except (BrokenPipeError, ConnectionResetError):
                        pass

            def do_GET(self) -> None:  # noqa: N802
                request = self.read_request()
                owner.record(request)
                if request.path != "/api/v1/observer-identity":
                    self.send_reply(Reply(404, owner.error("unknown-route")))
                    return
                self.send_reply(owner.next_identity_reply())

            def do_POST(self) -> None:  # noqa: N802
                request = self.read_request()
                owner.record(request)
                prefix = "/api/v1/observer-streams/"
                if request.path.startswith(prefix) and request.path.endswith("/confirmations"):
                    receipt = json.loads(request.body)["receipt"]
                    self.send_reply(Reply(200, owner.envelope({"schema_version": "shin.observer-confirmation.v1",
                        "server_id": owner.server_id, "stream_id": receipt["stream_id"],
                        "confirmed_sequence": receipt["sequence"], "retired_sequence": 0,
                        "producer_durability_verified": False, "shared_publication_mutated": False})))
                    return
                if request.path.startswith(prefix) and request.path.endswith("/reconciliation"):
                    self.send_reply(owner.next_reconciliation_reply(request))
                    return
                if not request.path.startswith(prefix) or not request.path.endswith("/submissions"):
                    self.send_reply(Reply(404, owner.error("unknown-route")))
                    return
                reply = owner.next_submission_reply(request)
                if reply.drop_after_commit:
                    self.close_connection = True
                    return
                try:
                    self.send_reply(reply)
                except (BrokenPipeError, ConnectionResetError):
                    self.close_connection = True

        class Server(http.server.ThreadingHTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self._server = Server(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, name="shin-delivery-fake", daemon=True)

    @property
    def port(self) -> int:
        return int(self._server.server_address[1])

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=3.0)
        if self._thread.is_alive():
            raise IntegrationFailure("controlled fake server did not stop")

    def record(self, request: Request) -> None:
        with self._lock:
            self.requests.append(request)

    def requests_for(self, method: str) -> list[Request]:
        with self._lock:
            return [request for request in self.requests if request.method == method]

    def error(self, code: str) -> bytes:
        return compact_json({"schema_version": SERVER_RESPONSE_SCHEMA, "tool": "shin-server", "version": SERVER_VERSION,
                             "stability": "development", "ok": False,
                             "error": {"code": code, "message": "Controlled protocol response."}})

    def envelope(self, result: dict[str, Any]) -> bytes:
        return compact_json({"schema_version": SERVER_RESPONSE_SCHEMA, "tool": "shin-server", "version": SERVER_VERSION,
                             "stability": "development", "ok": True, "result": result})

    def identity(self, *, server_id: str | None = None, principal_id: str = "reader") -> Reply:
        return Reply(200, self.envelope({"schema_version": "shin.observer-server-identity.v3",
                                        "server_id": server_id or self.server_id, "principal_id": principal_id,
                                        "submission_schema": "shin.observer-submission.v1",
                                        "acknowledgement_schema": "shin.observer-ack.v1",
                                        "confirmation_schema": "shin.observer-confirmation.v1",
                                        "reconciliation_schema": "shin.observer-reconcile-result.v1",
                                        "request_identity_header": "X-Shin-Server-ID",
                                        "verification_level": "service-observed", "source_verified": False,
                                        "shared_publication_mutated": False}))

    def next_identity_reply(self) -> Reply:
        with self._lock:
            return self.identity_replies.pop(0) if self.identity_replies else self.identity()

    def receipt(self, submission: dict[str, Any]) -> dict[str, Any]:
        payload = submission["payload"]
        return {"schema_version": "shin.observer-ack.v1", "server_id": self.server_id,
                "stream_id": submission["stream_id"], "sequence": submission["sequence"],
                "submission_id": submission["submission_id"], "payload_sha256": submission["payload_sha256"],
                "project_id": payload["project_id"], "workspace_id": payload["workspace_id"],
                "received_at": "2026-09-06T00:00:00.000000Z", "verification_level": "producer-observed",
                "server_source_verified": False, "shared_publication_mutated": False}

    def next_submission_reply(self, request: Request) -> Reply:
        try:
            submission = json.loads(request.body)
        except json.JSONDecodeError:
            return Reply(400, self.error("invalid-submission"))
        if not isinstance(submission, dict):
            return Reply(400, self.error("invalid-submission"))
        key = str(submission.get("submission_id", ""))
        with self._lock:
            reply = self.submission_replies.pop(0) if self.submission_replies else Reply(201)
            if reply.drop_after_commit:
                self.committed.setdefault(key, json.loads(json.dumps(submission)))
                return reply
            if reply.status in {200, 201} and not reply.body:
                replayed = key in self.committed
                self.committed.setdefault(key, json.loads(json.dumps(submission)))
                return Reply(reply.status, self.envelope({"receipt": self.receipt(submission), "replayed": replayed}), reply.headers,
                             False, reply.stall_seconds)
            return reply

    def next_reconciliation_reply(self, request: Request) -> Reply:
        try:
            probe = json.loads(request.body)
        except json.JSONDecodeError:
            return Reply(400, self.error("invalid-reconciliation"))
        if not isinstance(probe, dict):
            return Reply(400, self.error("invalid-reconciliation"))
        with self._lock:
            if self.reconciliation_replies:
                return self.reconciliation_replies.pop(0)
        acknowledged = probe.get("acknowledged_sequence")
        accepted = probe.get("accepted_sequence")
        pending = probe.get("next_submission")
        if not isinstance(acknowledged, int) or not isinstance(accepted, int) or accepted != acknowledged + 1 or not isinstance(pending, dict):
            return Reply(400, self.error("invalid-reconciliation"))
        return Reply(200, self.envelope({
            "schema_version": "shin.observer-reconcile-result.v1", "server_id": self.server_id,
            "stream_id": probe.get("stream_id"), "state": "send-next",
            "local_acknowledged_sequence": acknowledged, "local_accepted_sequence": accepted,
            "server_accepted_sequence": acknowledged, "confirmed_sequence": 0, "retired_sequence": 0,
            "retained_submissions": acknowledged, "retained_bytes": 0, "confirmation_required": False,
            "next_action": "send-next", "resume_allowed": True, "observer_state_mutated": False,
            "shared_publication_mutated": False, "source_verified": False, "local_state_verified": False,
            "verification_level": "service-observed",
        }))


@contextmanager
def fake_server(*, server_id: str = SERVER_ID) -> Iterator[ControlledProtocolServer]:
    server = ControlledProtocolServer(server_id=server_id)
    server.start()
    try:
        yield server
    finally:
        server.close()


class DeliveryHarness:
    def __init__(self, observer: Path, core: Path, report: Path) -> None:
        self.observer = observer
        self.core = core
        self.report = report
        self.checks: list[dict[str, Any]] = []
        self.started_at = time.monotonic()
        self._processes: list[subprocess.Popen[str]] = []
        self._root: Path | None = None

    @property
    def environment(self) -> dict[str, str]:
        environment = {"PATH": "", "LC_ALL": os.environ.get("LC_ALL", "C")}
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "ASAN_SYMBOLIZER_PATH"):
            if name in os.environ:
                environment[name] = os.environ[name]
        return environment

    def record(self, name: str, operation: Callable[[], None]) -> None:
        started = time.monotonic()
        try:
            operation()
        except BaseException as error:
            self.checks.append({"name": name, "ok": False,
                                "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
                                "failure": type(error).__name__,
                                "reason": str(error).replace(TOKEN, "[redacted]")[:2048] if isinstance(error, IntegrationFailure) else "fixture-error"})
            raise
        self.checks.append({"name": name, "ok": True, "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})

    def fixture(self, name: str) -> Fixture:
        if self._root is None:
            raise IntegrationFailure("temporary fixture root is unavailable")
        fixture = Fixture(self._root, name)
        config = fixture.config_data()
        config["outbox"] = {"enabled": True, "max_entries": 128, "max_bytes": 128 * 1024}
        fixture.write_config(config)
        return fixture

    def delivery_config(self, fixture: Fixture, port: int, *, server_id: str = SERVER_ID,
                        timeout_ms: int = 250, token: str = TOKEN) -> Path:
        token_path = fixture.base / "delivery.token"
        if token_path.exists():
            require_private_regular(token_path, "delivery token")
            if token_path.read_bytes() != token.encode("ascii") + b"\n":
                raise IntegrationFailure("fixture token unexpectedly changed")
        else:
            write_private(token_path, token.encode("ascii") + b"\n")
        config_path = fixture.base / f"delivery-{port}.json"
        write_private_json(config_path, {"schema_version": DELIVERY_CONFIG_SCHEMA, "host": "127.0.0.1", "port": port,
                                         "server_id": server_id, "principal_id": "reader", "token_file": str(token_path),
                                         "request_timeout_ms": timeout_ms})
        require_private_regular(token_path, "delivery token")
        require_private_regular(config_path, "delivery config")
        return config_path

    def run_cli(self, fixture: Fixture, command: str, delivery: Path | None = None, *extra: str,
                expect_success: bool = True, timeout: float = 8.0) -> tuple[dict[str, Any], subprocess.CompletedProcess[str]]:
        arguments = [str(self.observer), command, "--config", str(fixture.config)]
        # Keep the public order from the command contract: watch has its
        # watch-only bound before the optional delivery configuration.
        if command == "watch":
            arguments.extend(extra)
            if delivery is not None:
                arguments.extend(["--delivery-config", str(delivery)])
        else:
            if delivery is not None:
                arguments.extend(["--delivery-config", str(delivery)])
            arguments.extend(extra)
        process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env=self.environment, start_new_session=True)
        self._processes.append(process)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=3.0)
            raise IntegrationFailure(f"Observer {command} exceeded the test deadline") from error
        finally:
            if process in self._processes:
                self._processes.remove(process)
        completed = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
        rendered = stdout + stderr
        forbidden = [TOKEN]
        if delivery is not None:
            forbidden.extend([str(delivery), str(delivery.with_name("delivery.token"))])
        if any(value in rendered for value in forbidden):
            raise IntegrationFailure(f"Observer {command} leaked delivery credentials or paths")
        if expect_success and completed.returncode != 0:
            raise IntegrationFailure(f"Observer {command} failed: {completed.stderr}")
        if not expect_success and completed.returncode == 0:
            raise IntegrationFailure(f"Observer {command} unexpectedly succeeded")
        stream = completed.stdout if expect_success else completed.stderr
        if (expect_success and completed.stderr) or (not expect_success and completed.stdout):
            raise IntegrationFailure(f"Observer {command} used the wrong output stream")
        try:
            value = json.loads(stream)
        except json.JSONDecodeError as error:
            raise IntegrationFailure(f"Observer {command} did not emit one JSON response") from error
        assert_observer_envelope(value, expected_ok=expect_success)
        return value, completed

    def sample(self, fixture: Fixture) -> dict[str, Any]:
        value, _ = self.run_cli(fixture, "sample")
        return state_from_result(value["result"])

    def deliver(self, fixture: Fixture, delivery: Path, *, maximum: int | None = None,
                expect_failure: bool = False, timeout: float = 8.0) -> tuple[dict[str, Any], subprocess.CompletedProcess[str]]:
        extra = () if maximum is None else ("--max-submissions", str(maximum))
        value, completed = self.run_cli(fixture, "deliver", delivery, *extra, timeout=timeout)
        result = value["result"]
        failed = result.get("delivery_failed")
        if failed is not expect_failure or not isinstance(result.get("attempts_completed"), int) or \
                not isinstance(result.get("pending_drained"), bool):
            raise IntegrationFailure("deliver did not report its bounded attempt outcome")
        return state_from_result(result), completed

    @staticmethod
    def outbox(state: dict[str, Any]) -> dict[str, Any]:
        queue = state.get("outbox")
        expected = {"schema_version", "stream_id", "accepted_sequence", "acknowledged_sequence",
                    "acknowledged_observation_sequence", "last_receipt", "destination", "delivery", "entries",
                    "payload_bytes", "coalesced_observations", "rejected_observations",
                    "first_rejected_observation_sequence", "last_rejected_observation_sequence"}
        if not isinstance(queue, dict) or set(queue) != expected or queue.get("schema_version") != OUTBOX_SCHEMA:
            raise IntegrationFailure("state does not preserve the exact v2 delivery outbox schema")
        delivery = queue["delivery"]
        delivery_fields = {"attempts", "failures", "last_outcome", "last_attempt_at_ms", "last_result_at_ms", "last_duration_ms"}
        if not isinstance(queue["entries"], list) or not isinstance(delivery, dict) or set(delivery) != delivery_fields or \
                not isinstance(delivery["attempts"], int) or not isinstance(delivery["failures"], int) or \
                delivery["attempts"] < 0 or delivery["failures"] < 0 or delivery["failures"] > delivery["attempts"] or \
                not isinstance(delivery["last_duration_ms"], (int, float)) or delivery["last_duration_ms"] < 0:
            raise IntegrationFailure("delivery outbox queue is malformed")
        return queue

    @staticmethod
    def assert_destination(queue: dict[str, Any], port: int, *, server_id: str = SERVER_ID) -> None:
        destination = queue["destination"]
        if destination != {"host": "127.0.0.1", "port": port, "server_id": server_id, "principal_id": "reader"}:
            raise IntegrationFailure("delivery destination was not persisted as an immutable token-free identity")

    @staticmethod
    def assert_no_secret_leak(value: Any, fixture: Fixture, delivery: Path) -> None:
        rendered = json.dumps(value, sort_keys=True)
        if TOKEN in rendered or str(delivery) in rendered or str(delivery.with_name("delivery.token")) in rendered:
            raise IntegrationFailure("delivery token or token-file path leaked into Observer output or state")
        # The inherited Observer binding intentionally contains private local paths.
        # Submission packets and delivery metadata must never export those paths.
        if isinstance(value, dict):
            queue = value.get("outbox")
            if isinstance(queue, dict):
                exported = json.dumps({"entries": queue.get("entries"), "destination": queue.get("destination")}, sort_keys=True)
                if any(str(path) in exported for path in (fixture.root, fixture.out, fixture.profiles, fixture.state)):
                    raise IntegrationFailure("local source or state path leaked into a delivery packet")

    @staticmethod
    def assert_server_identity_header(request: Request, server_id: str = SERVER_ID) -> None:
        if request.server_id_headers != (server_id,):
            raise IntegrationFailure("Observer did not send exactly one configured Server identity header")

    @staticmethod
    def assert_request(request: Request, submission: dict[str, Any]) -> None:
        expected_path = f"/api/v1/observer-streams/{submission['stream_id']}/submissions"
        if request.path != expected_path or request.headers.get("authorization") != f"Bearer {TOKEN}" or \
                request.headers.get("content-type") != "application/json" or request.body != compact_json(submission):
            raise IntegrationFailure("Observer submission request did not preserve the exact authenticated packet")
        DeliveryHarness.assert_server_identity_header(request)
        if len(request.body) > 8192:
            raise IntegrationFailure("Observer sent a submission above the protocol body bound")

    def check_valid_ack_drain_and_monotonic_sequence(self) -> None:
        fixture = self.fixture("valid-ack")
        first = self.sample(fixture)
        initial = self.outbox(first)
        if initial["accepted_sequence"] != 1 or initial["acknowledged_sequence"] != 0 or len(initial["entries"]) != 1:
            raise IntegrationFailure("first observation was not durably queued with sequence one")
        first_packet = json.loads(json.dumps(initial["entries"][0]))
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            drained, completed = self.deliver(fixture, delivery, maximum=1)
            queue = self.outbox(drained)
            self.assert_destination(queue, server.port)
            if queue["accepted_sequence"] != 1 or queue["acknowledged_sequence"] != 1 or queue["entries"] or \
                    queue["last_receipt"] is None or queue["acknowledged_observation_sequence"] != first["sequence"]:
                raise IntegrationFailure("valid acknowledgement did not atomically drain the first submission")
            posts = server.requests_for("POST")
            identities = server.requests_for("GET")
            if len(identities) != 1 or len(posts) != 1:
                raise IntegrationFailure("delivery did not perform one identity handshake and one ordered POST")
            identity = identities[0]
            if identity.path != "/api/v1/observer-identity" or identity.body or identity.headers.get("authorization") != f"Bearer {TOKEN}":
                raise IntegrationFailure("delivery identity handshake was not the required authenticated GET")
            self.assert_server_identity_header(identity)
            self.assert_request(posts[0], first_packet)
            if queue["last_receipt"] != server.receipt(first_packet):
                raise IntegrationFailure("persisted receipt did not match the acknowledged submission")
            self.assert_no_secret_leak(completed.stdout, fixture, delivery)
            self.assert_no_secret_leak(drained, fixture, delivery)

            fixture.write_source("A changed document produces a later packet.\n")
            second = self.sample(fixture)
            next_packet = self.outbox(second)["entries"][0]
            if next_packet["sequence"] != 2 or self.outbox(second)["accepted_sequence"] != 2:
                raise IntegrationFailure("a drained queue did not retain monotonic accepted sequence")
            # Omission exercises the required default max-submissions bound.
            drained_second, _ = self.deliver(fixture, delivery)
            final_queue = self.outbox(drained_second)
            if final_queue["acknowledged_sequence"] != 2 or final_queue["entries"]:
                raise IntegrationFailure("second acknowledgement did not preserve monotonic drain state")
            posts = server.requests_for("POST")
            if len(posts) != 3 or not posts[1].path.endswith("/confirmations"):
                raise IntegrationFailure("second delivery emitted an unexpected POST count")
            if json.loads(posts[1].body) != {"receipt": drained["outbox"]["last_receipt"]}:
                raise IntegrationFailure("sender did not confirm its persisted prior receipt")
            self.assert_server_identity_header(posts[1])
            self.assert_request(posts[2], next_packet)

    def check_dropped_reply_replays_exact_packet_after_restart(self) -> None:
        fixture = self.fixture("dropped-reply")
        queued = self.outbox(self.sample(fixture))["entries"][0]
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            server.submission_replies.append(Reply(201, drop_after_commit=True))
            self.deliver(fixture, delivery, expect_failure=True)
            failed_state = json.loads((fixture.state / "state.json").read_text(encoding="utf-8"))
            failed_queue = self.outbox(failed_state)
            if failed_queue["entries"] != [queued] or failed_queue["acknowledged_sequence"] != 0:
                raise IntegrationFailure("lost reply rewrote or drained an unacknowledged packet")
            replayed, _ = self.deliver(fixture, delivery, maximum=1)
            queue = self.outbox(replayed)
            if queue["entries"] or queue["acknowledged_sequence"] != 1 or queue["last_receipt"] is None:
                raise IntegrationFailure("replayed acknowledgement did not drain the original packet")
            posts = server.requests_for("POST")
            if len(posts) != 2:
                raise IntegrationFailure("dropped reply command retried automatically instead of stopping once")
            self.assert_request(posts[0], queued)
            self.assert_request(posts[1], queued)
            if len(server.committed) != 1:
                raise IntegrationFailure("controlled peer did not receive the same submission identity on replay")

    def check_wrong_server_rejected_before_post(self) -> None:
        fixture = self.fixture("wrong-server")
        self.sample(fixture)
        with fake_server() as server:
            server.identity_replies.append(server.identity(server_id=OTHER_SERVER_ID))
            delivery = self.delivery_config(fixture, server.port)
            self.deliver(fixture, delivery, expect_failure=True)
            identities = server.requests_for("GET")
            if len(identities) != 1 or server.requests_for("POST"):
                raise IntegrationFailure("wrong server identity was not rejected before export")
            self.assert_server_identity_header(identities[0])
            state = json.loads((fixture.state / "state.json").read_text(encoding="utf-8"))
            queue = self.outbox(state)
            self.assert_destination(queue, server.port)
            if queue["entries"] == []:
                raise IntegrationFailure("wrong server identity drained a pending submission")

    def check_legacy_handshake_rejected_before_post(self) -> None:
        fixture = self.fixture("legacy-handshake")
        self.sample(fixture)
        with fake_server() as server:
            legacy = {
                "schema_version": "shin.observer-server-identity.v2", "server_id": SERVER_ID, "principal_id": "reader",
                "submission_schema": "shin.observer-submission.v1", "acknowledgement_schema": "shin.observer-ack.v1",
                "confirmation_schema": "shin.observer-confirmation.v1", "reconciliation_schema": "shin.observer-reconcile-result.v1",
                "verification_level": "service-observed", "source_verified": False, "shared_publication_mutated": False,
            }
            server.identity_replies.append(Reply(200, server.envelope(legacy)))
            delivery = self.delivery_config(fixture, server.port)
            self.deliver(fixture, delivery, expect_failure=True)
            identities = server.requests_for("GET")
            if len(identities) != 1 or server.requests_for("POST"):
                raise IntegrationFailure("legacy identity handshake was accepted before export")
            self.assert_server_identity_header(identities[0])

    def check_killed_sender_retains_pending_and_writer_fence(self) -> None:
        fixture = self.fixture("killed-sender")
        before = self.sample(fixture)
        packet = self.outbox(before)["entries"][0]
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port, timeout_ms=3000)
            server.submission_replies.append(Reply(201, stall_seconds=1.5))
            child = subprocess.Popen([str(self.observer), "deliver", "--config", str(fixture.config),
                                      "--delivery-config", str(delivery)], stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                     env=self.environment, start_new_session=True)
            self._processes.append(child)
            deadline = time.monotonic() + 3
            while not server.requests_for("POST") and time.monotonic() < deadline:
                time.sleep(0.005)
            if not server.requests_for("POST"):
                raise IntegrationFailure("sender did not reach the controlled commit boundary")
            busy, _ = self.run_cli(fixture, "deliver", delivery, expect_success=False)
            if busy.get("error", {}).get("code") != "writer-busy" or len(server.requests_for("POST")) != 1:
                raise IntegrationFailure("second sender crossed the single-writer fence")
            os.killpg(child.pid, signal.SIGKILL)
            child.communicate(timeout=3)
            self._processes.remove(child)
            persisted = json.loads((fixture.state / "state.json").read_text(encoding="utf-8"))
            if self.outbox(persisted)["entries"] != [packet] or self.outbox(persisted)["acknowledged_sequence"] != 0:
                raise IntegrationFailure("killed sender lost its unacknowledged packet")
            self.deliver(fixture, delivery)
            posts = server.requests_for("POST")
            if len(posts) != 2 or posts[0].body != posts[1].body:
                raise IntegrationFailure("restart did not resend the identical durable packet")

    def check_endpoint_binding_is_immutable(self) -> None:
        fixture = self.fixture("endpoint-binding")
        self.sample(fixture)
        with fake_server() as first:
            delivery = self.delivery_config(fixture, first.port)
            first.submission_replies.append(Reply(401, first.error("unauthorized")))
            self.deliver(fixture, delivery, expect_failure=True)
            bound = self.outbox(json.loads((fixture.state / "state.json").read_text(encoding="utf-8")))
            self.assert_destination(bound, first.port)
            with fake_server() as second:
                replacement = self.delivery_config(fixture, second.port)
                self.run_cli(fixture, "deliver", replacement, expect_success=False)
                if second.requests:
                    raise IntegrationFailure("changed endpoint was contacted after a destination became bound")
                if self.outbox(json.loads((fixture.state / "state.json").read_text(encoding="utf-8")))["entries"] == []:
                    raise IntegrationFailure("endpoint change altered pending delivery history")

    @staticmethod
    def assert_unchanged_read_only_state(fixture: Fixture, before: bytes, pending_before: bytes | None = None) -> None:
        state_path = fixture.state / "state.json"
        if state_path.read_bytes() != before:
            raise IntegrationFailure("read-only reconciliation changed committed state bytes")
        pending_path = fixture.state / "state.pending"
        if pending_before is None and pending_path.exists():
            raise IntegrationFailure("read-only reconciliation left pending recovery state")
        if pending_before is not None:
            require_private_regular(pending_path, "read-only pending state")
            if pending_path.read_bytes() != pending_before:
                raise IntegrationFailure("read-only reconciliation changed pending recovery bytes")

    def check_reconcile_requires_persisted_destination_before_network(self) -> None:
        fixture = self.fixture("reconcile-unbound")
        self.sample(fixture)
        before = (fixture.state / "state.json").read_bytes()
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            value, _ = self.run_cli(fixture, "reconcile", delivery, expect_success=False)
            if value.get("error", {}).get("code") != "delivery-binding-required" or server.requests:
                raise IntegrationFailure("unbound reconciliation did not fail before all network contact")
        self.assert_unchanged_read_only_state(fixture, before)

    def check_reconcile_matching_destination_is_read_only(self) -> None:
        fixture = self.fixture("reconcile-matching")
        self.sample(fixture)
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            server.submission_replies.append(Reply(401, server.error("unauthorized")))
            self.deliver(fixture, delivery, expect_failure=True)
            before = (fixture.state / "state.json").read_bytes()
            pending_path = fixture.state / "state.pending"
            pending_before = b"p3r-read-only-pending-sentinel\n"
            write_private(pending_path, pending_before)
            lock_path = fixture.state / "observer.lock"
            require_private_regular(lock_path, "Observer writer lock")
            lock_descriptor = os.open(lock_path, os.O_RDONLY | os.O_NOFOLLOW)
            requests_before = len(server.requests)
            try:
                fcntl.flock(lock_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                value, _ = self.run_cli(fixture, "reconcile", delivery)
            finally:
                fcntl.flock(lock_descriptor, fcntl.LOCK_UN)
                os.close(lock_descriptor)
            result = value["result"]
            requests = server.requests[requests_before:]
            if result.get("state") != "send-next" or result.get("resume_allowed") is not True or \
                    len(requests) != 2 or requests[0].method != "GET" or requests[1].method != "POST" or \
                    not requests[1].path.endswith("/reconciliation"):
                raise IntegrationFailure("matching reconciliation did not perform the bounded read-only probe")
            self.assert_server_identity_header(requests[0])
            self.assert_server_identity_header(requests[1])
            self.assert_destination(self.outbox(json.loads(before)), server.port)
        self.assert_unchanged_read_only_state(fixture, before, pending_before)

    def check_reconcile_rejects_different_destination_before_network(self) -> None:
        fixture = self.fixture("reconcile-mismatch")
        self.sample(fixture)
        with fake_server() as first:
            bound = self.delivery_config(fixture, first.port)
            first.submission_replies.append(Reply(401, first.error("unauthorized")))
            self.deliver(fixture, bound, expect_failure=True)
            before = (fixture.state / "state.json").read_bytes()
            with fake_server() as second:
                replacement = self.delivery_config(fixture, second.port)
                value, _ = self.run_cli(fixture, "reconcile", replacement, expect_success=False)
                if value.get("error", {}).get("code") != "delivery-binding-mismatch" or second.requests:
                    raise IntegrationFailure("different reconciliation destination was contacted")
        self.assert_unchanged_read_only_state(fixture, before)

    def check_reconcile_rejects_resumable_retired_head(self) -> None:
        fixture = self.fixture("reconcile-retired-head")
        self.sample(fixture)
        fixture.write_source("A second packet makes the retired-head response contradictory.\n")
        self.sample(fixture)
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            server.submission_replies.append(Reply(401, server.error("unauthorized")))
            self.deliver(fixture, delivery, expect_failure=True)
            before = (fixture.state / "state.json").read_bytes()
            queue = self.outbox(json.loads(before))
            if queue["acknowledged_sequence"] != 0 or queue["accepted_sequence"] != 2 or \
                    [entry.get("sequence") for entry in queue["entries"]] != [1, 2]:
                raise IntegrationFailure("retired-head fixture did not retain the required local queue")
            server.reconciliation_replies.append(Reply(200, server.envelope({
                "schema_version": "shin.observer-reconcile-result.v1", "server_id": SERVER_ID,
                "stream_id": queue["stream_id"], "state": "replay-head",
                "local_acknowledged_sequence": 0, "local_accepted_sequence": 2,
                "server_accepted_sequence": 2, "confirmed_sequence": 2, "retired_sequence": 1,
                "retained_submissions": 1, "retained_bytes": 0, "confirmation_required": False,
                "next_action": "replay-head", "resume_allowed": True, "observer_state_mutated": False,
                "shared_publication_mutated": False, "source_verified": False, "local_state_verified": False,
                "verification_level": "service-observed",
            })))
            value, _ = self.run_cli(fixture, "reconcile", delivery, expect_success=False)
            if value.get("error", {}).get("code") != "protocol-invalid":
                raise IntegrationFailure("resumable retired-head response was not rejected as protocol-invalid")
        self.assert_unchanged_read_only_state(fixture, before)

    def check_malformed_ack_preserves_queue(self) -> None:
        fixture = self.fixture("malformed-ack")
        before = self.outbox(self.sample(fixture))
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            malformed = server.envelope({"receipt": {"schema_version": "shin.observer-ack.v1"}, "replayed": False})
            server.submission_replies.append(Reply(201, malformed))
            self.deliver(fixture, delivery, expect_failure=True)
            after = self.outbox(json.loads((fixture.state / "state.json").read_text(encoding="utf-8")))
            if after["entries"] != before["entries"] or after["acknowledged_sequence"] != 0:
                raise IntegrationFailure("malformed acknowledgement changed the durable queue")

    def check_redirect_and_oversized_identity_are_not_accepted(self) -> None:
        fixture = self.fixture("redirect")
        self.sample(fixture)
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            server.identity_replies.append(Reply(302, b"{}", (("Location", "http://127.0.0.1:9/elsewhere"),)))
            self.deliver(fixture, delivery, expect_failure=True)
            if len(server.requests_for("GET")) != 1 or server.requests_for("POST"):
                raise IntegrationFailure("redirect response was followed or permitted export")

        fixture = self.fixture("oversized-body")
        self.sample(fixture)
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            server.identity_replies.append(Reply(200, b"{" + b"x" * MAX_RESPONSE_BYTES + b"}"))
            self.deliver(fixture, delivery, expect_failure=True)
            if server.requests_for("POST"):
                raise IntegrationFailure("oversized identity body permitted a submission POST")

    def check_stall_deadline_and_watch_local_progress(self) -> None:
        fixture = self.fixture("stall")
        baseline = self.sample(fixture)
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port, timeout_ms=100)
            server.submission_replies.extend([Reply(201, stall_seconds=0.7)] * 4)
            started = time.monotonic()
            stalled, _ = self.deliver(fixture, delivery, expect_failure=True, timeout=1.5)
            if self.outbox(stalled)["entries"] == [] or time.monotonic() - started > 1.5:
                raise IntegrationFailure("stalled delivery did not respect its command deadline")

            started = time.monotonic()
            watched, _ = self.run_cli(fixture, "watch", delivery, "--samples", "5", timeout=3.0)
            elapsed = time.monotonic() - started
            final = state_from_result(watched["result"])
            queue = self.outbox(final)
            if final["sequence"] < baseline["sequence"] + 5 or elapsed > 2.0:
                raise IntegrationFailure("watch did not continue local observations while HTTP was stalled")
            if not 1 <= queue["delivery"].get("attempts", 0) <= 4 or len(server.requests_for("POST")) > 4:
                raise IntegrationFailure("stalled watch did not keep delivery attempts bounded")

    def check_watch_retries_transient_backoff_and_blocks_policy_failures(self) -> None:
        fixture = self.fixture("transient-watch")
        self.sample(fixture)
        reserve = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        reserve.bind(("127.0.0.1", 0))
        unavailable_port = int(reserve.getsockname()[1])
        reserve.close()
        delivery = self.delivery_config(fixture, unavailable_port, timeout_ms=100)
        watched, _ = self.run_cli(fixture, "watch", delivery, "--samples", "10", timeout=4.0)
        queue = self.outbox(state_from_result(watched["result"]))
        attempts = queue["delivery"].get("attempts")
        if not isinstance(attempts, int) or attempts < 2 or attempts > 6 or queue["entries"] == []:
            raise IntegrationFailure("transient watch failure did not retry with a bounded backoff")

        for status in (401, 409):
            fixture = self.fixture(f"blocked-{status}")
            self.sample(fixture)
            with fake_server() as server:
                delivery = self.delivery_config(fixture, server.port)
                server.submission_replies.append(Reply(status, server.error("blocked")))
                watched, _ = self.run_cli(fixture, "watch", delivery, "--samples", "5", timeout=3.0)
                queue = self.outbox(state_from_result(watched["result"]))
                if len(server.requests_for("POST")) != 1 or queue["delivery"].get("attempts") != 1 or \
                        queue["entries"] == []:
                    raise IntegrationFailure(f"HTTP {status} was retried during watch or drained the queue")

    def check_delivery_config_and_cli_bounds(self) -> None:
        fixture = self.fixture("input-bounds")
        packet = self.outbox(self.sample(fixture))["entries"][0]
        with fake_server() as server:
            delivery = self.delivery_config(fixture, server.port)
            # sample accepts the delivery configuration too; this is command-surface
            # coverage, not a claim that the controlled peer is the real Server.
            sampled, _ = self.run_cli(fixture, "sample", delivery)
            queue = self.outbox(state_from_result(sampled["result"]))
            posts = server.requests_for("POST")
            if queue["entries"] or queue["acknowledged_sequence"] != packet["sequence"] or \
                    queue["acknowledged_observation_sequence"] != packet["payload"]["observer_sequence"] or \
                    queue["last_receipt"] != server.receipt(packet) or len(server.requests_for("GET")) != 1 or len(posts) != 1:
                raise IntegrationFailure("sample --delivery-config did not atomically persist its acknowledgement")
            self.assert_destination(queue, server.port)
            self.assert_request(posts[0], packet)
            for maximum in ("0", "129"):
                self.run_cli(fixture, "deliver", delivery, "--max-submissions", maximum, expect_success=False)
            invalid = json.loads(delivery.read_text(encoding="utf-8"))
            invalid["unexpected"] = True
            replacement = fixture.base / "invalid-delivery.json"
            write_private_json(replacement, invalid)
            self.run_cli(fixture, "deliver", replacement, expect_success=False)
            token = fixture.base / "invalid-token"
            write_private(token, b"contains space")
            invalid = json.loads(delivery.read_text(encoding="utf-8"))
            invalid["token_file"] = str(token)
            replacement = fixture.base / "invalid-token-delivery.json"
            write_private_json(replacement, invalid)
            self.run_cli(fixture, "deliver", replacement, expect_success=False)
            delivery.chmod(0o644)
            try:
                self.run_cli(fixture, "deliver", delivery, expect_success=False)
            finally:
                delivery.chmod(0o600)

    def check_observer_envelope_validator_rejections(self) -> None:
        success = {
            "schema_version": RESPONSE_SCHEMA, "tool": "shin-observer", "version": OBSERVER_VERSION,
            "core_version": CORE_VERSION, "stability": OBSERVER_STABILITY, "ok": True, "result": {},
        }
        error = {
            "schema_version": RESPONSE_SCHEMA, "tool": "shin-observer", "version": OBSERVER_VERSION,
            "core_version": CORE_VERSION, "stability": OBSERVER_STABILITY, "ok": False,
            "error": {"code": "protocol-invalid", "message": "Rejected."},
        }
        rejected = {
            "wrong-tool": ({**success, "tool": "other-tool"}, True),
            "wrong-version": ({**success, "version": "0.0.0-dev.1"}, True),
            "wrong-core-version": ({**success, "core_version": "0.0.0-dev.1"}, True),
            "wrong-stability": ({**success, "stability": "development" if OBSERVER_STABILITY == "stable" else "stable"}, True),
            "extra-key": ({**success, "unexpected": True}, True),
            "mixed-success": ({**success, "error": error["error"]}, True),
            "mixed-error": ({**error, "result": success["result"]}, False),
        }
        for name, (value, expected_ok) in rejected.items():
            try:
                assert_observer_envelope(value, expected_ok=expected_ok)
            except IntegrationFailure:
                continue
            raise IntegrationFailure(f"Observer envelope validator accepted {name}")

    def run(self) -> None:
        with tempfile.TemporaryDirectory(prefix="shin-observer-delivery-") as temporary:
            self._root = Path(temporary).resolve()
            self.record("valid-ack-drain-and-monotonic-sequence", self.check_valid_ack_drain_and_monotonic_sequence)
            self.record("dropped-reply-replay-after-restart", self.check_dropped_reply_replays_exact_packet_after_restart)
            self.record("killed-sender-and-single-writer-recovery", self.check_killed_sender_retains_pending_and_writer_fence)
            self.record("wrong-server-before-post", self.check_wrong_server_rejected_before_post)
            self.record("legacy-handshake-before-post", self.check_legacy_handshake_rejected_before_post)
            self.record("changed-endpoint-after-binding", self.check_endpoint_binding_is_immutable)
            self.record("reconcile-unbound-before-network", self.check_reconcile_requires_persisted_destination_before_network)
            self.record("reconcile-matching-destination-read-only", self.check_reconcile_matching_destination_is_read_only)
            self.record("reconcile-different-destination-before-network", self.check_reconcile_rejects_different_destination_before_network)
            self.record("reconcile-retired-head-protocol-invalid", self.check_reconcile_rejects_resumable_retired_head)
            self.record("malformed-ack-preserves-queue", self.check_malformed_ack_preserves_queue)
            self.record("redirect-and-oversized-body", self.check_redirect_and_oversized_identity_are_not_accepted)
            self.record("stalled-delivery-and-local-watch-progress", self.check_stall_deadline_and_watch_local_progress)
            self.record("watch-transient-backoff-and-policy-block", self.check_watch_retries_transient_backoff_and_blocks_policy_failures)
            self.record("delivery-config-and-cli-bounds", self.check_delivery_config_and_cli_bounds)
            self.record("observer-envelope-validator-rejections", self.check_observer_envelope_validator_rejections)

    def cleanup(self) -> None:
        for process in list(self._processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                process.communicate(timeout=3.0)
            except subprocess.SubprocessError:
                pass
            self._processes.remove(process)

    def write_report(self, ok: bool) -> None:
        metrics = {"elapsed_ms": round((time.monotonic() - self.started_at) * 1000, 3),
                   "check_count": len(self.checks), "passed_count": sum(1 for check in self.checks if check["ok"]),
                   "controlled_peer_only": True}
        payload = {"ok": ok, "checks": self.checks, "metrics": metrics}
        descriptor = os.open(self.report, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(payload, output, separators=(",", ":"), sort_keys=True)
            output.write("\n")


def executable(value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
        raise IntegrationFailure(f"--{label} must be an absolute executable file")
    return path.resolve()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observer", required=True, help="absolute shin-observer binary")
    parser.add_argument("--core", required=True, help="absolute shin-context binary; validated but never used to build fixtures")
    parser.add_argument("--report", required=True, help="new absolute JSON report path")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        observer = executable(args.observer, "observer")
        core = executable(args.core, "core")
        report = Path(args.report)
        if not report.is_absolute() or report.exists() or not report.parent.is_dir():
            raise IntegrationFailure("--report must be a new absolute path with an existing parent directory")
        harness = DeliveryHarness(observer, core, report)
        try:
            harness.run()
        except BaseException as error:
            harness.cleanup()
            harness.write_report(False)
            print(f"error: Observer delivery integration test failed: {type(error).__name__}", file=sys.stderr)
            return 1
        harness.cleanup()
        harness.write_report(True)
        print("Observer delivery integration test passed; report written to --report")
        return 0
    except IntegrationFailure as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
