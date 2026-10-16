#!/usr/bin/env python3
"""Real TLS failures and background key lifecycle against the native provider."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import select
import ssl
import subprocess
import tempfile
import threading
import time


class Fixture(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, certificate, key):
        super().__init__(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        self.socket = context.wrap_socket(self.socket, server_side=True)
        self.issuer = f"https://127.0.0.1:{self.server_port}/issuer"
        self.origin = f"https://127.0.0.1:{self.server_port}"
        self.mode = "normal"
        self.keys = {}
        self.requests = []
        self.lock = threading.Lock()
        self.entered = threading.Event()
        self.release = threading.Event()

    def handle_error(self, request, client_address):
        pass  # Aborted TLS/body transfers are intentional test cases.

    def configure(self, mode, keys=None):
        with self.lock:
            self.mode = mode
            if keys is not None:
                self.keys = keys

    def count(self):
        with self.lock:
            return len(self.requests)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        fixture = self.server
        with fixture.lock:
            mode, keys = fixture.mode, fixture.keys
            fixture.requests.append(self.path)
        discovery = self.path == "/issuer/.well-known/openid-configuration"
        if not discovery and self.path != "/keys":
            self.send_error(404)
            return
        if mode == "stall":
            fixture.entered.set()
            fixture.release.wait(4)
        status = 503 if mode == "unavailable" else 200
        metadata = {"issuer": fixture.issuer, "jwks_uri": fixture.origin + "/keys"}
        if mode == "wrong-issuer":
            metadata["issuer"] += "/other"
        if mode == "cross-origin":
            metadata["jwks_uri"] = "https://127.0.0.1:1/keys"
        if mode == "http-keys":
            metadata["jwks_uri"] = "http://127.0.0.1:1/keys"
        if mode == "userinfo-keys":
            metadata["jwks_uri"] = fixture.origin.replace("https://", "https://user@") + "/keys"
        if mode == "relative-keys":
            metadata["jwks_uri"] = "/keys"
        body = json.dumps(metadata if discovery else keys).encode()
        if mode == "duplicate" and discovery:
            body = b'{"issuer":"untrusted",' + body[1:]
        if mode == "large-body":
            body = b"x" * 65537
        if mode == "invalid-keys" and not discovery:
            body = b'{"keys":[]}'
        self.send_response(302 if mode == "redirect" else status)
        if mode == "redirect":
            self.send_header("Location", fixture.origin + "/keys")
        if mode == "large-header":
            self.send_header("X-Large", "x" * 16384)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass


class Probe:
    def __init__(self, binary, options):
        self.process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.initial = self.call(options)

    def call(self, value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()
        if not select.select([self.process.stdout], [], [], 12)[0]:
            raise RuntimeError("Native probe response deadline")
        return json.loads(self.process.stdout.readline())

    def close(self):
        try:
            if not self.initial.get("started"):
                if self.process.wait(timeout=5) != 1:
                    raise RuntimeError("Invalid provider configuration unexpectedly admitted")
                return {}
            if self.process.poll() is None:
                result = self.call({"command": "quit"})
                if self.process.wait(timeout=5) != 0:
                    raise RuntimeError("Native provider shutdown failed")
                return result
            if self.process.returncode != 1:
                raise RuntimeError("Native provider unexpectedly exited")
            return {}
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()


def run(binary, report_path):
    checks, probes = [], []
    observations = {}
    started = time.monotonic()

    def check(name, value):
        if not value:
            raise RuntimeError("Failed: " + name)
        checks.append(name)

    def await_status(probe, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while True:
            status = probe.call({"command": "status"})
            if predicate(status):
                return status
            if time.monotonic() >= deadline:
                raise RuntimeError("Provider state deadline")
            threading.Event().wait(0.02)

    report = {"schema_version": "shin.oidc-provider-evidence.v1", "checks": checks, "observations": observations}
    with binary.open("rb") as source:
        report["probe_sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
    try:
        with tempfile.TemporaryDirectory(prefix="shin-oidc-tls-") as temporary:
            certificate, key = Path(temporary) / "tls.crt", Path(temporary) / "tls.key"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-keyout", str(key), "-out", str(certificate), "-subj", "/CN=localhost",
                            "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True, timeout=15)
            fixture = Fixture(certificate, key)
            thread = threading.Thread(target=fixture.serve_forever, daemon=True)
            thread.start()
            try:
                made = subprocess.run([str(binary), "fixtures"], input=json.dumps({"issuer": fixture.issuer})+"\n",
                                      capture_output=True, text=True, check=True, timeout=15)
                data = json.loads(made.stdout)
                fixture.configure("normal", data["a"])
                options = {"issuer": fixture.issuer, "ca_file": str(certificate)}

                def start(**overrides):
                    probe = Probe(binary, {**options, **overrides})
                    probes.append(probe)
                    return probe

                for override in ({"issuer": "http://127.0.0.1"}, {"issuer": "https://user@host"},
                                 {"issuer": fixture.issuer+"#fragment"}, {"issuer": fixture.issuer+"?query"},
                                 {"timeout_ms": 0}, {"interval_seconds": 0}, {"lifetime_seconds": 301}):
                    probe = start(**override)
                    check("invalid-config-refused", probe.initial.get("code") == "oidc-provider-config-invalid")
                    probe.close(); probes.remove(probe)

                for override in ({"ca_file": ""}, {"issuer": fixture.issuer.replace("127.0.0.1", "localhost")}):
                    before = fixture.count()
                    probe = start(**override)
                    status = await_status(probe, lambda s: s["attempts"] >= 1)
                    check("tls-trust-or-host-refused", not status["ready"] and status["last_error"] == "oidc-transport-failed")
                    check("tls-refusal-before-http", fixture.count() == before)
                    probe.close(); probes.remove(probe)

                for mode, expected in (("wrong-issuer", "oidc-issuer-mismatch"), ("cross-origin", "oidc-jwks-origin-denied"),
                                       ("http-keys", "oidc-jwks-uri-invalid"), ("userinfo-keys", "oidc-jwks-uri-invalid"),
                                       ("relative-keys", "oidc-jwks-uri-invalid"), ("redirect", "oidc-http-failed"),
                                       ("duplicate", "oidc-metadata-invalid"), ("large-body", "oidc-response-too-large"),
                                       ("large-header", "oidc-response-too-large"), ("invalid-keys", "oidc-keys-invalid")):
                    fixture.configure(mode)
                    before = fixture.count()
                    probe = start()
                    status = await_status(probe, lambda s: s["attempts"] >= 1)
                    check(mode+"-refused", not status["ready"] and status["last_error"] == expected)
                    if mode in ("cross-origin", "http-keys", "userinfo-keys", "relative-keys", "redirect"):
                        check(mode+"-no-followup-fetch", fixture.count() == before+1)
                    probe.close(); probes.remove(probe)

                fixture.configure("normal", data["a"])
                probe = start(interval_seconds=60, lifetime_seconds=300)
                check("initial-background-refresh", probe.call({"command": "wait"})["ready"])
                before = fixture.count()
                observations["warm_verification"] = probe.call({"command": "verify", "token": data["token_a"], "count": 500})
                check("valid-token", observations["warm_verification"]["ok"])
                for _ in range(20):
                    check("unknown-key-refused", probe.call({"command": "verify", "token": data["token_b"]})["code"] == "oidc-token-invalid")
                check("verification-never-fetches", fixture.count() == before)
                observations["transport"] = probe.call({"command": "status"})
                check("idle-stop-interrupts-refresh-wait", probe.close()["shutdown_ms"] < 500)
                probes.remove(probe)

                probe = start()
                check("rotation-initial-ready", probe.call({"command": "wait"})["ready"])
                previous = probe.call({"command": "status"})["successes"]
                fixture.configure("normal", data["both"])
                await_status(probe, lambda s: s["successes"] > previous)
                check("rotation-overlap-old", probe.call({"command": "verify", "token": data["token_a"]})["ok"])
                check("rotation-overlap-new", probe.call({"command": "verify", "token": data["token_b"]})["ok"])
                previous = probe.call({"command": "status"})["successes"]
                fixture.configure("normal", data["b"])
                await_status(probe, lambda s: s["successes"] > previous)
                check("removed-key-refused", probe.call({"command": "verify", "token": data["token_a"]})["code"] == "oidc-token-invalid")
                fixture.configure("invalid-keys")
                await_status(probe, lambda s: s["failures"] > 0)
                check("bad-refresh-keeps-unexpired-good-keys", probe.call({"command": "verify", "token": data["token_b"]})["ok"])
                await_status(probe, lambda s: not s["ready"])
                check("failed-refresh-does-not-extend-ttl", probe.call({"command": "verify", "token": data["token_b"]})["code"] == "oidc-keys-expired")
                fixture.configure("normal")
                check("background-recovery", probe.call({"command": "wait", "timeout_ms": 8000})["ready"])
                probe.close(); probes.remove(probe)

                fixture.configure("normal", data["a"])
                probe = start(lifetime_seconds=10)
                check("stall-initial-ready", probe.call({"command": "wait"})["ready"])
                fixture.configure("stall")
                check("background-fetch-entered", fixture.entered.wait(3))
                verified = probe.call({"command": "verify", "token": data["token_a"], "count": 100})
                check("blocked-refresh-does-not-block-verification", verified["ok"])
                status = await_status(probe, lambda s: s["failures"] > 0)
                check("network-deadline", status["last_error"] == "oidc-refresh-timeout" and status["last_refresh_ms"] < 1500)
                observations["timeout_ms"] = status["last_refresh_ms"]
                fixture.entered.clear()
                check("retry-fetch-entered", fixture.entered.wait(4))
                check("active-stop-bounded", probe.close()["shutdown_ms"] < 1500)
                probes.remove(probe)
                report["ok"] = True
            finally:
                fixture.release.set()
                for probe in probes:
                    probe.close()
                fixture.shutdown()
                fixture.server_close()
                thread.join(timeout=2)
    except Exception as error:
        report["ok"] = False
        report["failure"] = str(error) if isinstance(error, RuntimeError) else type(error).__name__
    report["elapsed_seconds"] = round(time.monotonic()-started,3)
    if report_path:
        with report_path.open("x") as output:
            json.dump(report,output,indent=2)
            output.write("\n")
    print(json.dumps(report))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.report and args.report.exists():
        parser.error("Existing evidence must not be overwritten")
    os.umask(0o077)
    raise SystemExit(run(args.probe.resolve(strict=True), args.report))
