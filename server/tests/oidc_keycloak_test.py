#!/usr/bin/env python3
"""Owned, disposable Keycloak fixture. Never uses personal accounts or projects."""
import argparse
import base64
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser


IMAGE = "quay.io/keycloak/keycloak:26.8.0@sha256:27b3230fbabb8c9ff3c66c864cf09dcd8a41aca82e5100679d109ef0eeb05741"


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class LoginForm(HTMLParser):
    def __init__(self):
        super().__init__()
        self.action = None

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "form" and values.get("id") == "kc-form-login":
            self.action = values.get("action")


def docker(*args, timeout=120):
    return subprocess.run(["docker", *args], check=True, capture_output=True, text=True, timeout=timeout).stdout.strip()


def realm(password):
    return {
        "realm": "shin-oidc-test", "enabled": True, "sslRequired": "all",
        "registrationAllowed": False, "resetPasswordAllowed": False,
        "accessTokenLifespan": 300, "revokeRefreshToken": True, "refreshTokenMaxReuse": 0,
        "clients": [{
            "clientId": "shin-ce", "protocol": "openid-connect", "publicClient": True,
            "standardFlowEnabled": True, "directAccessGrantsEnabled": False,
            "implicitFlowEnabled": False, "serviceAccountsEnabled": False,
            "redirectUris": ["http://127.0.0.1:18765/callback"], "webOrigins": [],
            "attributes": {"pkce.code.challenge.method": "S256", "access.token.header.type.rfc9068": "true",
                           "oauth2.device.authorization.grant.enabled": "true"},
            "protocolMappers": [
                {"name": "resource-audience", "protocol": "openid-connect", "protocolMapper": "oidc-audience-mapper",
                 "config": {"included.custom.audience": "shin-server", "access.token.claim": "true", "id.token.claim": "false"}},
                {"name": "rfc9068-client", "protocol": "openid-connect", "protocolMapper": "oidc-hardcoded-claim-mapper",
                 "config": {"claim.name": "client_id", "claim.value": "shin-ce", "jsonType.label": "String",
                            "access.token.claim": "true", "id.token.claim": "false", "userinfo.token.claim": "false"}},
            ],
        }],
        "users": [{"username": "fixture-user", "enabled": True, "emailVerified": True,
                   "firstName": "Fixture", "lastName": "User", "email": "fixture@example.test",
                   "credentials": [{"type": "password", "value": password, "temporary": False}]}],
    }


def request(opener, url, body=None, headers=None):
    data = None if body is None else urllib.parse.urlencode(body).encode()
    try:
        result = opener.open(urllib.request.Request(url, data=data, headers=headers or {}), timeout=10)
    except urllib.error.HTTPError as error:
        result = error
    with result:
        payload = result.read(131073)
        if len(payload) > 131072:
            raise RuntimeError("Fixture HTTP body exceeded bound")
        return result.code, result.headers, payload.decode()


def run(binary, report_path):
    name = "shin-oidc-test-" + secrets.token_hex(6)
    checks = []
    timings = {}
    started = time.monotonic()
    created = False

    def check(name, condition):
        if not condition:
            raise RuntimeError("Failed: " + name)
        checks.append(name)
        print("check: " + name, flush=True)

    report = {"schema_version": "shin.oidc-keycloak-evidence.v1", "image": IMAGE, "checks": checks,
              "boundary": "test-only verifier and IdP; not Shin HTTP login/device/directory authorization"}
    with binary.open("rb") as probe_file:
        report["probe_sha256"] = hashlib.file_digest(probe_file, "sha256").hexdigest()
    try:
        with tempfile.TemporaryDirectory(prefix="shin-oidc-") as temporary:
            path = Path(temporary) / "realm.json"
            password = secrets.token_urlsafe(32)
            path.write_text(json.dumps(realm(password)))
            # The private parent prevents host traversal; Keycloak's unprivileged
            # container UID needs read access to this single read-only bind mount.
            path.chmod(0o644)
            certificate, key = Path(temporary) / "tls.crt", Path(temporary) / "tls.key"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-keyout", str(key), "-out", str(certificate), "-subj", "/CN=localhost",
                            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"],
                           check=True, capture_output=True, timeout=15)
            certificate.chmod(0o644)
            key.chmod(0o644)
            tls = ssl.create_default_context(cafile=str(certificate))
            docker("create", "--name", name, "--memory", "768m", "--cpus", "2", "--pids-limit", "256",
                   "--publish", "127.0.0.1::8443", "--mount", f"type=bind,source={path},target=/opt/keycloak/data/import/realm.json,readonly",
                   "--mount", f"type=bind,source={certificate},target=/opt/keycloak/conf/tls.crt,readonly",
                   "--mount", f"type=bind,source={key},target=/opt/keycloak/conf/tls.key,readonly",
                   "--env", "JAVA_OPTS_KC_HEAP=-Xms64m -Xmx384m", IMAGE, "start-dev", "--import-realm",
                   "--http-enabled=false", "--https-certificate-file=/opt/keycloak/conf/tls.crt",
                   "--https-certificate-key-file=/opt/keycloak/conf/tls.key")
            created = True
            docker("start", name)
            endpoint = docker("port", name, "8443/tcp")
            if not endpoint.startswith("127.0.0.1:") or "\n" in endpoint:
                raise RuntimeError("Fixture port not loopback-only")
            origin = "https://" + endpoint
            issuer = origin + "/realms/shin-oidc-test"
            plain = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=tls), NoRedirect())
            deadline = time.monotonic() + 120
            while True:
                try:
                    status, _, body = request(plain, issuer + "/.well-known/openid-configuration")
                    if status == 200:
                        break
                except (OSError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("Keycloak readiness deadline")
                threading.Event().wait(0.25)
            discovery = json.loads(body)
            untrusted = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
            try:
                request(untrusted, issuer + "/.well-known/openid-configuration")
            except urllib.error.URLError as error:
                check("untrusted-tls-refused", isinstance(error.reason, ssl.SSLCertVerificationError))
            else:
                check("untrusted-tls-refused", False)
            check("discovery-exact-issuer", discovery["issuer"] == issuer)
            for field in ("authorization_endpoint", "token_endpoint", "jwks_uri", "revocation_endpoint", "device_authorization_endpoint"):
                check("discovery-origin-" + field, discovery[field].startswith(issuer + "/"))
            status, _, body = request(plain, discovery["jwks_uri"])
            check("jwks-retrieved", status == 200)
            jwks = json.loads(body)

            def authorize():
                verifier = secrets.token_urlsafe(48)
                challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
                state, nonce = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
                redirect = "http://127.0.0.1:18765/callback"
                params = {"client_id": "shin-ce", "response_type": "code", "scope": "openid",
                          "redirect_uri": redirect, "state": state, "nonce": nonce,
                          "code_challenge": challenge, "code_challenge_method": "S256"}
                browser = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=tls),
                    urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()), NoRedirect())
                status, _, page = request(browser, discovery["authorization_endpoint"] + "?" + urllib.parse.urlencode(params))
                form = LoginForm()
                form.feed(page)
                check("browser-login-form", status == 200 and form.action is not None and form.action.startswith(issuer + "/"))
                status, headers, page = request(browser, form.action,
                    {"username": "fixture-user", "password": password, "credentialId": ""},
                    {"Origin": origin, "Referer": discovery["authorization_endpoint"]})
                location = headers.get("Location", "")
                if status != 302 or not location.startswith(redirect + "?"):
                    target = urllib.parse.urlsplit(location)
                    report["callback_diagnostic"] = {"status": status, "origin": target.scheme + "://" + target.netloc,
                                                     "path": target.path, "query_keys": sorted(urllib.parse.parse_qs(target.query))}
                    report["callback_diagnostic"]["known_errors"] = [message for message in (
                        "Cookie not found", "Invalid username or password", "Invalid parameter", "HTTPS required",
                        "Session not active", "Invalid code", "Missing Origin", "Restart login") if message.lower() in page.lower()]
                check("browser-exact-callback", status == 302 and location.startswith(redirect + "?"))
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(location).query)
                check("browser-state", query.get("state") == [state] and len(query.get("code", [])) == 1)
                return {"grant_type": "authorization_code", "client_id": "shin-ce", "redirect_uri": redirect,
                        "code": query["code"][0], "code_verifier": verifier}

            exchange = authorize()
            status, _, body = request(plain, discovery["token_endpoint"], exchange)
            check("authorization-code-pkce", status == 200)
            tokens = json.loads(body)

            def probe(token, expected=True, **overrides):
                payload = {"issuer": issuer, "audience": "shin-server", "client_id": "shin-ce", "jwks": jwks, "token": token,
                           "provider": {"ca_file": str(certificate)}}
                payload.update(overrides)
                result = subprocess.run([str(binary)], input=json.dumps(payload), text=True, capture_output=True, timeout=15)
                parsed = json.loads(result.stdout)
                check("native-access-admitted" if expected else "native-access-refused",
                      result.returncode == (0 if expected else 1) and parsed["ok"] == expected)
                return parsed

            verified = probe(tokens["access_token"])
            check("native-signed-session-present", bool(verified["session_id"]))
            check("native-provider-discovery-ready", verified["provider"]["ready"] and verified["provider"]["successes"] == 1)
            report["native_provider"] = verified["provider"]
            timings.update({k: v for k, v in verified.items() if k.startswith("verify_us_") or k == "samples"})
            probe(tokens["id_token"], False)
            probe(tokens["access_token"], False, audience="other-server")
            probe(tokens["access_token"], False, issuer=issuer + "-other")
            probe(tokens["access_token"], False, client_id="other-client")
            status, _, body = request(plain, discovery["token_endpoint"], exchange)
            check("authorization-code-replay-refused", status == 400 and json.loads(body)["error"] == "invalid_grant")
            wrong = authorize()
            wrong["code_verifier"] = secrets.token_urlsafe(48)
            status, _, body = request(plain, discovery["token_endpoint"], wrong)
            check("wrong-pkce-refused", status == 400 and json.loads(body)["error"] == "invalid_grant")
            # Use a fresh grant: replay defense may invalidate its previous session.
            status, _, body = request(plain, discovery["token_endpoint"], authorize())
            check("fresh-grant", status == 200)
            original = json.loads(body)
            original_session = probe(original["access_token"])["session_id"]
            check("new-login-distinct-session", bool(original_session) and original_session != verified["session_id"])
            refresh = {"grant_type": "refresh_token", "client_id": "shin-ce", "refresh_token": original["refresh_token"]}
            status, _, body = request(plain, discovery["token_endpoint"], refresh)
            check("refresh-rotation", status == 200 and json.loads(body)["refresh_token"] != original["refresh_token"])
            rotated = json.loads(body)
            check("refresh-retains-signed-session", probe(rotated["access_token"])["session_id"] == original_session)
            status, _, body = request(plain, discovery["token_endpoint"], refresh)
            check("refresh-reuse-refused", status == 400 and json.loads(body)["error"] == "invalid_grant")
            status, _, body = request(plain, discovery["token_endpoint"], authorize())
            check("revocation-grant", status == 200)
            revoke = json.loads(body)
            status, _, _ = request(plain, discovery["revocation_endpoint"],
                {"client_id": "shin-ce", "token": revoke["refresh_token"], "token_type_hint": "refresh_token"})
            check("refresh-revoked", status == 200)
            status, _, body = request(plain, discovery["token_endpoint"],
                {"grant_type": "refresh_token", "client_id": "shin-ce", "refresh_token": revoke["refresh_token"]})
            check("revoked-refresh-refused", status == 400 and json.loads(body)["error"] == "invalid_grant")
            # Signature validation alone does NOT detect logout/account/device
            # revocation. This assertion preserves that explicit boundary.
            probe(revoke["access_token"])
            check("revocation-still-needs-shin-live-admission", True)
            device_verifier = secrets.token_urlsafe(48)
            device_challenge = base64.urlsafe_b64encode(hashlib.sha256(device_verifier.encode()).digest()).decode().rstrip("=")
            status, _, body = request(plain, discovery["device_authorization_endpoint"],
                {"client_id": "shin-ce", "scope": "openid", "code_challenge": device_challenge, "code_challenge_method": "S256"})
            device = json.loads(body)
            report["device_response"] = {"status": status, "expires_in": device.get("expires_in"),
                                         "interval": device.get("interval"), "error": device.get("error")}
            if status != 200:
                # This request contains only the public client ID and scope.
                report["device_response"]["description"] = device.get("error_description", "")[:160]
            check("device-flow-advertised", status == 200 and device.get("expires_in", 0) > 0 and device.get("interval", 0) > 0)
            status, _, body = request(plain, discovery["token_endpoint"],
                {"grant_type": "urn:ietf:params:oauth:grant-type:device_code", "client_id": "shin-ce",
                 "device_code": device["device_code"], "code_verifier": device_verifier})
            check("device-flow-pending-not-authorized", status == 400 and json.loads(body)["error"] == "authorization_pending")
            report["container_memory"] = docker("stats", "--no-stream", "--format", "{{.MemUsage}}", name)
            report["ok"] = True
    except Exception as error:
        report["ok"] = False
        # Never report raw HTTP bodies, tokens, command stdout or credentials.
        report["failure"] = str(error) if isinstance(error, RuntimeError) else type(error).__name__
    finally:
        if created:
            docker("rm", "--force", name)
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        report["verification"] = timings
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with report_path.open("x") as output:
            json.dump(report, output, indent=2)
            output.write("\n")
    print(json.dumps({"ok": report["ok"], "checks": len(checks), "report": str(report_path)}), flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Choose a new report path; existing evidence is not overwritten")
    os.umask(0o077)
    raise SystemExit(run(args.probe.resolve(strict=True), args.report.resolve()))
