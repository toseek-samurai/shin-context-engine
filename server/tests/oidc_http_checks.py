"""Native OIDC HTTP admission with TLS, live PostgreSQL and personal source routes."""
from concurrent.futures import ThreadPoolExecutor
import json
import http.client
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from integration_test import HOST, PORT
from oidc_provider_test import Fixture
from publication_test_support import require


def check_oidc_http(s):
    original = s.runtime_config_path.read_bytes()
    config = json.loads(original)
    # These owners exist in the scoped fixture but have no static credentials.
    require(not any(p['id'] in ('scoped-alice', 'scoped-bob') for p in config['principals']),
            'OIDC fixture accidentally depends on static user credentials')
    paths = {who: '/api/v1/personal/tenants/' + tenant + '/projects/' + project + '/workspaces/primary'
             for who, tenant, project in [('alice', 'scope-a', 'visible'), ('bob', 'scope-b', 'other')]}
    routes = {who: paths[who] + '/routes/' + route for who, route in
              [('alice', 'visible-retrieval'), ('bob', 'other-retrieval')]}
    with tempfile.TemporaryDirectory(prefix='shin-oidc-http-') as tmp:
        certificate, key = Path(tmp) / 'tls.crt', Path(tmp) / 'tls.key'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-keyout', str(key), '-out', str(certificate), '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=IP:127.0.0.1'], check=True, capture_output=True, timeout=15)
        provider = Fixture(certificate, key)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        sessions = [{'sub': who, 'sid': sid} for who, sid in
                    [('alice', 'same-session'), ('bob', 'same-session'), ('alice', 'second-session'),
                     ('unknown', 'same-session'), ('alice', '')]]
        sessions += [{'sub': 'alice', 'sid': 'same-session', 'aud': 'wrong'},
                     {'sub': 'alice', 'sid': 'same-session', 'exp': int(time.time()) - 60},
                     {'sub': 'alice', 'sid': 'same-session', 'client_id': 'wrong'},
                     {'sub': 'alice', 'sid': 'same-session', 'roles': ['admin']},
                     {'sub': 'alice', 'sid': 'fresh-session'},
                     {'sub': 'alice', 'sid': 'same-session', 'iss': 'https://wrong.example.test'}]
        made = subprocess.run([str(s.binary.parent / 'shin-server-oidc-provider-probe'), 'fixtures'],
                              input=json.dumps({'issuer': provider.issuer, 'sessions': sessions}) + '\n',
                              capture_output=True, text=True, check=True, timeout=15)
        data = json.loads(made.stdout)
        tokens = dict(zip(['alice', 'bob', 'alice2', 'unknown', 'no-sid', 'bad-aud', 'expired',
                           'bad-client', 'fake-admin', 'fresh', 'bad-issuer'], data['tokens']))
        tokens['bad-signature'] = tokens['alice'][:-10] + ('A' if tokens['alice'][-10] != 'A' else 'B') + tokens['alice'][-9:]
        provider.configure('normal', data['keys'])
        config['oidc'] = {'issuer': provider.issuer, 'audience': 'shin-server', 'client_id': 'shin-ce',
                          'ca_file': str(certificate)}
        config['preparations']['enabled'] = True
        s.replace_private_json(s.runtime_config_path, config)

        def call(method, path, body=None, who='alice', status=200, key=None):
            headers = s.auth_headers('admin') if who == 'admin' else {
                'Authorization': 'Bearer ' + tokens[who], 'X-Shin-Server-ID': s.server_id}
            if body is not None: headers['Content-Type'] = 'application/json'
            if key: headers['Idempotency-Key'] = key
            result = s.expect_status(status, method, path, headers=headers,
                                    body=json.dumps(body).encode() if body is not None else None)
            return result.get('result', result.get('error'))

        def ready():
            deadline = time.monotonic() + 8
            while True:
                code, reply = s.request('GET', '/api/v1/identity/account', headers={
                    'Authorization': 'Bearer ' + tokens['unknown'], 'X-Shin-Server-ID': s.server_id})
                if code == 401: return
                require(code == 503 and time.monotonic() < deadline, 'OIDC startup did not become available')
                threading.Event().wait(.02)

        def brief(who='alice', status=200):
            return call('POST', routes[who] + '/brief', {'query': 'Routes'}, who, status)

        try:
            s.start_server()
            ready()
            for who in ('alice', 'bob'):
                call('POST', '/api/v1/identity/accounts/' + who + '/external',
                     {'issuer': provider.issuer, 'subject': who}, 'admin')
            # Explicit maintenance can admit a new dynamic principal without a token config entry.
            call('POST', '/api/v1/identity/accounts',
                 {'id': 'oidc-new', 'principal_id': 'oidc-new-principal', 'display_name': 'New account'}, 'admin')
            for who in ('alice', 'bob', 'unknown', 'no-sid', 'bad-aud', 'expired', 'bad-client', 'bad-issuer', 'bad-signature'):
                call('GET', '/api/v1/identity/account', who=who, status=401)
            for who in ('unknown', 'no-sid', 'bad-aud', 'expired', 'bad-client', 'bad-issuer', 'bad-signature', 'admin'):
                call('POST', '/api/v1/identity/device-session', {'display_name': 'Laptop'}, who, 401)
            require(s.scalar("SELECT count(*) FROM shin_server.identity_devices WHERE account_id IN ('alice','bob')") == '0',
                    'ordinary or refused requests implicitly registered devices')
            devices = {}
            for who in ('alice', 'bob', 'alice2'):
                registered = call('POST', '/api/v1/identity/device-session', {'display_name': 'Same laptop'}, who)
                devices[who] = registered['device']['id']
                replay = call('POST', '/api/v1/identity/device-session', {'display_name': 'Changed'}, who)
                require(replay['replayed'] and replay['device'] == registered['device'], 'login retry changed device')
            require(len(set(devices.values())) == 3, 'equal label/session across users merged devices')
            call('POST', '/api/v1/identity/device-session', {'display_name': 'Laptop', 'sid': 'forged'}, status=400)
            for who in ('alice', 'bob'):
                require(brief(who)['evidence']['ok'], 'OIDC personal retrieval failed')
                other = 'bob' if who == 'alice' else 'alice'
                call('POST', routes[other] + '/brief', {'query': 'Routes'}, who, 404)
                call('GET', '/api/v1/identity/accounts/' + other + '/devices', who=who, status=404)
                call('GET', '/api/v1/cache', who=who, status=403)
                call('GET', '/api/v1/identity/accounts', who=who, status=403)
            call('GET', '/api/v1/identity/accounts', who='fake-admin', status=403)
            with ThreadPoolExecutor(max_workers=4) as pool:
                replies = list(pool.map(brief, ['alice', 'bob'] * 8))
            require(all(r['tenant_id'] == ('scope-a' if i % 2 == 0 else 'scope-b') for i, r in enumerate(replies)),
                    'pooled JWT requests crossed owner scope')
            samples = []
            for _ in range(20):
                started = time.perf_counter_ns()
                brief()
                samples.append((time.perf_counter_ns() - started) / 1_000_000)
            s.oidc_http_measurement = {'samples': len(samples), 'p50_ms': sorted(samples)[9],
                                       'p95_ms': sorted(samples)[18], 'scope': 'warm personal Brief with live JWT/device admission'}
            connection = http.client.HTTPConnection(HOST, PORT, timeout=4)
            try:
                for who in ('alice', 'bob', 'alice2', 'bob'):
                    connection.request('GET', '/api/v1/identity/account', headers={
                        'Authorization': 'Bearer ' + tokens[who], 'X-Shin-Server-ID': s.server_id})
                    response = connection.getresponse()
                    body = json.loads(response.read())
                    require(response.status == 200 and body['result']['id'] == ('alice' if who == 'alice2' else who),
                            'keep-alive request reused previous principal')
            finally:
                connection.close()
            # Worker must resolve directory authority even when the user has no static credential.
            for who in ('alice', 'bob'):
                base = call('GET', routes[who] + '/activation', who=who)
                job = call('POST', routes[who] + '/preparations', {'expected_snapshot_id': base['snapshot_id']},
                           who, 202, key='oidc-build')['job']['id']
                path = paths[who] + '/preparations/' + str(job)
                deadline = time.monotonic() + 20
                while True:
                    observed = call('GET', path, who=who)
                    if observed['state'] in ('prepared', 'failed', 'cancelled'): break
                    require(time.monotonic() < deadline, 'dynamic preparation deadline')
                    threading.Event().wait(.05)
                require(observed['state'] == 'prepared', 'dynamic principal could not prepare: ' + str(observed))
                plan = call('GET', path + '/retention', who=who)
                call('POST', path + '/retention', {'expected_plan_id': plan['plan_id']}, who)
            for table in ('identity_accounts', 'identity_external_bindings'):
                column = 'id' if table == 'identity_accounts' else 'account_id'
                s.psql("UPDATE shin_server." + table + " SET enabled=false WHERE " + column + "='alice'")
                call('GET', '/api/v1/identity/account', status=401)
                call('POST', '/api/v1/identity/device-session', {'display_name': 'Laptop'}, 'fresh', 401)
                s.psql("UPDATE shin_server." + table + " SET enabled=true WHERE " + column + "='alice'")
                brief()
            call('POST', '/api/v1/identity/accounts/alice/devices/' + devices['alice'] + '/revoke',
                 {'expected_revision': 1}, 'alice2')
            call('GET', '/api/v1/identity/account', status=401)
            call('POST', '/api/v1/identity/device-session', {'display_name': 'Laptop'}, status=401)
            call('GET', '/api/v1/identity/account', who='alice2')
            s.stop_server(expect_clean=True)
            s.start_server()
            ready()
            call('GET', '/api/v1/identity/account', status=401)
            call('GET', '/api/v1/identity/account', who='alice2')
            brief('bob')
            # Unavailable initial keys fail closed while maintenance remains usable.
            s.stop_server(expect_clean=True)
            provider.configure('unavailable')
            s.start_server()
            call('GET', '/api/v1/identity/account', who='bob', status=503)
            call('GET', '/health/ready', who='admin')
        finally:
            s.stop_server(expect_clean=False)
            s.runtime_config_path.write_bytes(original)
            s.psql("DELETE FROM shin_server.identity_devices WHERE account_id IN ('alice','bob'); "
                   "DELETE FROM shin_server.identity_external_bindings WHERE account_id IN ('alice','bob')")
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=5)
