# Server: implemented capabilities and the path to a personal preview

Assessment of the public preparation tree, 2026-10-16. Server is
`2.0.0-dev.71`. This is a source assessment, not a deployed-service qualification
or an announcement of remote client support.

The backend already contains much of the personal workspace machinery. The
remaining work joins it into a supported installation and a complete native
client workflow. Local Shin remains usable independently of this work.

## What exists in source

| Capability | Implementation and evidence boundary |
| --- | --- |
| Accounts, tenant/project directory, personal workspace ownership | [HTTP routes](../server/src/http.cpp), [identity directory](../server/src/identity_directory.cpp), database migrations and isolation fixtures |
| External identity and device sessions | [OIDC provider](../server/src/oidc_provider.cpp), [external bindings](../server/src/identity_external.cpp), [device registration and revocation](../server/src/identity_devices.cpp) |
| Personal workspace preparation, activation, search, Brief and Pack | Personal API routes in [http.cpp](../server/src/http.cpp), workspace-scoped workers and authorization |
| Observer ingestion, reconciliation, retention and history | [Observer storage](../server/src/observer_store.cpp) and personal observer-stream routes |
| Recovery, task checkpoints and request journal | Recovery commands, scoped checkpoint and journal APIs, migration/recovery fixtures |

An OIDC access token is verified before resolving the account and enrolled
device. The operator must first bind the external issuer/subject to an enabled
account. This is not automatic signup by email. Revoked devices and disabled
accounts/bindings must lose access.

The public candidate has passed the 30-test Server CTest suite on macOS ARM64
and Linux ARM64 during preparation. Those tests do **not** replace PostgreSQL
integration, a real identity-provider workflow, or a deployed client-to-server
test. The [release contract](machine/shin-server-release-closure-v1.json) still
marks Server unqualified.

## What blocks an ordinary user's remote workflow

1. **Native login and session lifecycle.** `shin-ce identity` only reads local
   identity. There is no native browser/device login, secure refresh-token
   storage, refresh, logout, or expired/revoked-session recovery. Backend device
   registration is already present; the user-facing client is missing.
2. **Remote transport and personal routes.** Observer's
   [delivery config](../observer/src/delivery_config.cpp) accepts only
   `127.0.0.1`, with a static token file capped at 256 bytes. Its
   [transport](../observer/src/delivery_transport.cpp) uses the older
   `/api/v1/observer-streams/...` routes. OIDC sessions are admitted to personal
   routes. Changing the hostname or supplying a JWT cannot bridge this gap.
   Native CLI/MCP need an explicit remote workspace binding and retrieval path,
   in addition to Observer delivery.
3. **Supported deployment and first setup.** The Server listener is intentionally
   loopback-only. A remote edition needs a documented HTTPS boundary, a
   reproducible package/service deployment, PostgreSQL provisioning and
   migrations, identity-provider configuration, and initial account/workspace
   setup. The development Compose file starts PostgreSQL, not a complete
   deployable Shin application.
4. **One coherent user flow.** Connect a server, authenticate, select a personal
   workspace, explicitly connect its local source, prepare/activate context,
   query it, and reconnect after interruption. CLI and Monitor should expose
   the same identity, workspace and failure state. The current local Monitor
   is not a Server administration UI.
5. **Proof with persistent state.** Exercise isolation, authorization revocation,
   interruption/retry, expiry/refresh, migration, restart, backup/restore and
   upgrade on the exact candidate. Preserve workspace bindings, indexes,
   receipts and histories throughout. Unit results alone do not cover this.

## A bounded first release

The smallest useful Server preview is one operator-managed server with
operator-provisioned personal accounts, isolated workspaces, and one supported
native client platform. Self-service signup, shared knowledge publishing,
cross-account collaboration, a web administration console, and every desktop
platform need not block that preview.

Suggested implementation order:

1. Make integration infrastructure independently disposable. The current
   [test harness](../server/tests/integration_test.py) resets `shin_server` and
   [Compose](../server/infra/compose.yml) hardcodes its database volume. A new
   checkout or state directory does not provide database isolation. Use a
   dedicated disposable Docker environment, or add explicit instance ownership
   and isolation before running against a shared daemon.
2. Qualify the existing PostgreSQL/HTTP backend and real OIDC provider fixtures
   on the public inputs. Resolve failures before expanding client behavior.
   The [Keycloak fixture](../server/tests/oidc_keycloak_test.py) tests the verifier
   and IdP; it explicitly does not prove native Shin login or directory access.
3. Deliver deployment/bootstrap and the native login/session slice together
   with a supported HTTPS trust path.
4. Connect Observer and CLI/MCP to personal workspace routes, then expose the
   connection and recovery states in Monitor.
5. Run a two-account, two-device end-to-end proof and an upgrade/restore proof;
   ship setup instructions that reproduce that exact supported topology.

This is a finite integration effort with substantial backend reuse. It is more
than packaging: native authentication, remote delivery/retrieval, and lifecycle
proof remain real implementation work. The local app can ship earlier without
claiming these remote capabilities.
