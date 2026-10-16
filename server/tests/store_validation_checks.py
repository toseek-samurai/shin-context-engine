"""Sidecar drift checks for Store schema validation.

The integration harness owns process and fixture setup. This module changes
reviewed fixture catalog metadata and restores each injected change.
"""

from __future__ import annotations

import json
import re
from typing import Any, Protocol


SCHEMA_UNSUPPORTED = "schema-unsupported"
SERVER_IDENTITY_MISMATCH = "server-identity-mismatch"
READY_PATH = "/health/ready"
LIVE_PATH = "/health/live"
PROTECTED_PATH = "/api/v1/projects"

# One representative table per independently validated Store domain.
DOMAIN_PROBES = (
    ("activation", "active_routes"),
    ("workspace", "workspaces"),
    ("observer", "observer_streams"),
    ("audit", "audit_history"),
    ("recovery", "server_recovery"),
    ("identity", "server_identity"),
    ("base-preparations", "preparations"),
    ("publication-proposals", "publication_proposals"),
    ("publication-resolutions", "publication_resolutions"),
    ("publication-heads", "publication_heads"),
    ("publication-withdrawals", "publication_withdrawals"),
    ("workspace-publication-history", "workspace_publication_history"),
)


class Suite(Protocol):
    server: Any
    server_id: str

    def psql(self, sql: str) -> str: ...

    def scalar(self, sql: str) -> str: ...

    def request(
        self,
        method: str,
        target: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
        include_server_identity: bool = True,
    ) -> tuple[int, dict[str, Any]]: ...

    def expect_status(
        self,
        expected: int,
        method: str,
        target: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes | None = None,
    ) -> dict[str, Any]: ...

    def wait_for_response(
        self,
        target: str,
        expected_status: int,
        *,
        headers: dict[str, str] | None = None,
    ) -> dict[str, Any]: ...

    def auth_headers(self, role: str) -> dict[str, str]: ...


def probe_column(domain: str) -> str:
    """Return a fixed, SQL-safe column name owned by this sidecar."""
    if domain not in {name for name, _ in DOMAIN_PROBES}:
        raise ValueError(f"unknown Store validation domain: {domain}")
    return "p3e10_store_validation_probe_" + domain.replace("-", "_")


def _assert_server_running(suite: Suite, process: Any) -> None:
    if process is None or suite.server is not process or process.poll() is not None:
        raise RuntimeError("Store schema validation changed the Server process")


def _error_code(response: dict[str, Any]) -> str | None:
    error = response.get("error")
    return error.get("code") if isinstance(error, dict) and isinstance(error.get("code"), str) else None


def _assert_failure_code(response: dict[str, Any], expected: str, context: str) -> None:
    actual = _error_code(response)
    if actual != expected:
        raise RuntimeError(f"{context} returned {actual!r}; expected {expected!r}")


def _assert_fail_closed(suite: Suite, process: Any, context: str, expected_code: str = SCHEMA_UNSUPPORTED) -> None:
    """Require both the readiness and a normal Store request to reject drift."""
    ready = suite.wait_for_response(READY_PATH, 503, headers=suite.auth_headers("admin"))
    _assert_failure_code(ready, expected_code, context)
    status, protected = suite.request("GET", PROTECTED_PATH, headers=suite.auth_headers("admin"))
    if status != 503:
        raise RuntimeError(f"{context} did not fail closed for a Store request: HTTP {status}")
    _assert_failure_code(protected, expected_code, context)
    suite.expect_status(200, "GET", LIVE_PATH)
    _assert_server_running(suite, process)


def _assert_restored(suite: Suite, process: Any, context: str) -> None:
    ready = suite.wait_for_response(READY_PATH, 200, headers=suite.auth_headers("admin"))
    if ready.get("result") != {"status": "ready", "scope": "store", "server_id": suite.server_id}:
        raise RuntimeError(f"{context} did not restore the expected readiness envelope")
    suite.expect_status(200, "GET", LIVE_PATH)
    _assert_server_running(suite, process)


def _probe_column_drift(suite: Suite, process: Any, domain: str, table: str) -> None:
    column = probe_column(domain)
    suite.psql(f"ALTER TABLE shin_server.{table} ADD COLUMN {column} BOOLEAN")
    try:
        # Preserve the existing workspace API spelling while checking the exact refusal.
        code = "unsupported-schema" if domain == "workspace" else SCHEMA_UNSUPPORTED
        _assert_fail_closed(suite, process, f"{domain} column drift", code)
    finally:
        suite.psql(f"ALTER TABLE shin_server.{table} DROP COLUMN {column}")
    _assert_restored(suite, process, f"{domain} column drift")


def _catalog_object(suite: Suite, query: str, context: str) -> dict[str, str]:
    raw = suite.scalar(query)
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{context} catalog definition was not JSON") from error
    if not isinstance(value, dict) or set(value) != {"name", "definition"}:
        raise RuntimeError(f"{context} catalog definition has the wrong shape")
    name = value["name"]
    definition = value["definition"]
    if not isinstance(name, str) or not isinstance(definition, str) or not name or not definition:
        raise RuntimeError(f"{context} catalog definition is incomplete")
    return {"name": name, "definition": definition}


def _constraint_drift(suite: Suite, process: Any) -> None:
    constraint = _catalog_object(
        suite,
        "SELECT json_build_object('name', quote_ident(conname), "
        "'definition', pg_get_constraintdef(oid, true))::text "
        "FROM pg_constraint WHERE conrelid = 'shin_server.audit_history'::regclass "
        "AND contype = 'c' ORDER BY conname LIMIT 1",
        "audit constraint",
    )
    suite.psql(f"ALTER TABLE shin_server.audit_history DROP CONSTRAINT {constraint['name']}")
    try:
        _assert_fail_closed(suite, process, "audit constraint drift")
    finally:
        suite.psql(
            f"ALTER TABLE shin_server.audit_history ADD CONSTRAINT {constraint['name']} {constraint['definition']}"
        )
    _assert_restored(suite, process, "audit constraint drift")


def _index_drift(suite: Suite, process: Any) -> None:
    index = _catalog_object(
        suite,
        "SELECT json_build_object('name', quote_ident(c.relname), "
        "'definition', pg_get_indexdef(i.indexrelid))::text "
        "FROM pg_index AS i JOIN pg_class AS c ON c.oid = i.indexrelid "
        "WHERE i.indrelid = 'shin_server.active_routes'::regclass "
        "AND NOT i.indisprimary AND i.indisvalid ORDER BY c.relname LIMIT 1",
        "activation index",
    )
    suite.psql(f"DROP INDEX shin_server.{index['name']}")
    try:
        _assert_fail_closed(suite, process, "activation index drift")
    finally:
        suite.psql(index["definition"])
    _assert_restored(suite, process, "activation index drift")


def _preparation_scope_drift(suite: Suite, process: Any) -> None:
    constraint = _catalog_object(suite,
        "SELECT json_build_object('name',quote_ident(conname),'definition',pg_get_constraintdef(oid))::text "
        "FROM pg_constraint WHERE conrelid='shin_server.preparations'::regclass "
        "AND conname='preparation_workspace_admission_check'", "preparation workspace constraint")
    table = "shin_server.preparations"
    suite.psql(f"ALTER TABLE {table} DROP CONSTRAINT {constraint['name']}; "
               f"ALTER TABLE {table} ADD CONSTRAINT {constraint['name']} CHECK (true)")
    try:
        _assert_fail_closed(suite, process, "preparation workspace constraint weakening")
    finally:
        suite.psql(f"ALTER TABLE {table} DROP CONSTRAINT {constraint['name']}; "
                   f"ALTER TABLE {table} ADD CONSTRAINT {constraint['name']} {constraint['definition']}")
    _assert_restored(suite, process, "preparation workspace constraint weakening")


def _request_key_index_drift(suite: Suite, process: Any) -> None:
    for name in ("preparations_legacy_request_unique", "preparations_workspace_request_unique",
                 "publication_proposals_legacy_request_unique", "publication_proposals_workspace_request_unique",
                 "preparations_workspace_cursor_idx"):
        definition = suite.scalar(f"SELECT pg_get_indexdef('shin_server.{name}'::regclass)")
        suite.psql(f"DROP INDEX shin_server.{name}")
        try:
            _assert_fail_closed(suite, process, "request-key uniqueness index missing")
        finally:
            suite.psql(definition)
        _assert_restored(suite, process, "request-key uniqueness index restored")
    suite.psql("ALTER TABLE shin_server.preparations ADD CONSTRAINT legacy_key_regression UNIQUE(principal_id,idempotency_key)")
    try:
        _assert_fail_closed(suite, process, "unexpected global request-key uniqueness")
    finally:
        suite.psql("ALTER TABLE shin_server.preparations DROP CONSTRAINT legacy_key_regression")
    _assert_restored(suite, process, "unexpected global request-key uniqueness removed")


def _preparation_relational_scope_drift(suite: Suite, process: Any) -> None:
    for name in ('preparation_scope_identity_check', 'preparation_scope_project_fk', 'preparation_scope_workspace_fk'):
        definition = suite.scalar("SELECT pg_get_constraintdef(oid) FROM pg_constraint "
                                  f"WHERE conrelid='shin_server.preparations'::regclass AND conname='{name}'")
        suite.psql(f"ALTER TABLE shin_server.preparations DROP CONSTRAINT {name}")
        try:
            _assert_fail_closed(suite, process, "preparation relational scope missing")
        finally:
            suite.psql(f"ALTER TABLE shin_server.preparations ADD CONSTRAINT {name} {definition}")
        _assert_restored(suite, process, "preparation relational scope restored")


def _workspace_request_key_drift(suite: Suite, process: Any) -> None:
    suite.psql("CREATE UNIQUE INDEX workspace_request_old_scope ON shin_server.workspace_requests(principal_id,key)")
    try:
        _assert_fail_closed(suite, process, "legacy workspace request uniqueness", "unsupported-schema")
    finally:
        suite.psql("DROP INDEX shin_server.workspace_request_old_scope")
    _assert_restored(suite, process, "legacy workspace request uniqueness")
    for definition in ("PRIMARY KEY(principal_id,key)",
                       "PRIMARY KEY(principal_id,project_id,key) DEFERRABLE INITIALLY DEFERRED"):
        suite.psql("ALTER TABLE shin_server.workspace_requests DROP CONSTRAINT workspace_requests_pkey, "
                   f"ADD CONSTRAINT workspace_requests_pkey {definition}")
        try:
            _assert_fail_closed(suite, process, "workspace request primary key drift", "unsupported-schema")
        finally:
            suite.psql("ALTER TABLE shin_server.workspace_requests DROP CONSTRAINT workspace_requests_pkey, "
                       "ADD CONSTRAINT workspace_requests_pkey PRIMARY KEY(principal_id,project_id,key)")
        _assert_restored(suite, process, "workspace request primary key drift")


def _checkpoint_namespace_drift(suite: Suite, process: Any) -> None:
    definition = suite.scalar("SELECT pg_get_indexdef('shin_server.checkpoint_slots_workspace_unique'::regclass)")
    suite.psql("DROP INDEX shin_server.checkpoint_slots_workspace_unique; "
               "CREATE UNIQUE INDEX checkpoint_slots_workspace_unique ON shin_server.checkpoint_slots(project_id,owner_id,slot_id) "
               "WHERE scope_workspace_id IS NOT NULL")
    try:
        _assert_fail_closed(suite, process, "checkpoint workspace uniqueness")
    finally:
        suite.psql("DROP INDEX shin_server.checkpoint_slots_workspace_unique; " + definition)
    _assert_restored(suite, process, "checkpoint workspace uniqueness")
    constraint = suite.scalar("SELECT pg_get_constraintdef(oid) FROM pg_constraint "
                              "WHERE conrelid='shin_server.checkpoint_slots'::regclass AND conname='checkpoint_slots_scope_check'")
    suite.psql("ALTER TABLE shin_server.checkpoint_slots DROP CONSTRAINT checkpoint_slots_scope_check, "
               "ADD CONSTRAINT checkpoint_slots_scope_check CHECK(true)")
    try:
        _assert_fail_closed(suite, process, "checkpoint scope consistency")
    finally:
        suite.psql("ALTER TABLE shin_server.checkpoint_slots DROP CONSTRAINT checkpoint_slots_scope_check, "
                   "ADD CONSTRAINT checkpoint_slots_scope_check " + constraint)
    _assert_restored(suite, process, "checkpoint scope consistency")


def _multiple_domain_drift(suite: Suite, process: Any) -> None:
    selected = (("activation", "active_routes"), ("observer", "observer_streams"))
    added: list[tuple[str, str]] = []
    try:
        for domain, table in selected:
            suite.psql(f"ALTER TABLE shin_server.{table} ADD COLUMN {probe_column(domain)} BOOLEAN")
            added.append((domain, table))
        _assert_fail_closed(suite, process, "multiple Store domains drift")
    finally:
        for domain, table in reversed(added):
            suite.psql(f"ALTER TABLE shin_server.{table} DROP COLUMN {probe_column(domain)}")
    _assert_restored(suite, process, "multiple Store domains drift")


def _publication_scope_drift(suite: Suite, process: Any) -> None:
    for table, name in (("publication_proposals", "publication_history_workspace_check"),
                        ("workspace_publication_history", "workspace_publication_history_history_epoch_check")):
        constraint = _catalog_object(suite,
            "SELECT json_build_object('name',quote_ident(conname),'definition',pg_get_constraintdef(oid))::text "
            f"FROM pg_constraint WHERE conrelid='shin_server.{table}'::regclass AND conname='{name}'",
            "publication history scope constraint")
        suite.psql(f"ALTER TABLE shin_server.{table} DROP CONSTRAINT {constraint['name']}; "
                   f"ALTER TABLE shin_server.{table} ADD CONSTRAINT {constraint['name']} CHECK (true)")
        try:
            _assert_fail_closed(suite, process, "publication history scope constraint weakening")
        finally:
            suite.psql(f"ALTER TABLE shin_server.{table} DROP CONSTRAINT {constraint['name']}; "
                       f"ALTER TABLE shin_server.{table} ADD CONSTRAINT {constraint['name']} {constraint['definition']}")
        _assert_restored(suite, process, "publication history scope constraint weakening")


def _missing_relation_drift(suite: Suite, process: Any) -> None:
    suite.psql("ALTER TABLE shin_server.active_routes RENAME TO p3e10_missing_active_routes")
    try:
        _assert_fail_closed(suite, process, "missing activation relation")
    finally:
        suite.psql("ALTER TABLE shin_server.p3e10_missing_active_routes RENAME TO active_routes")
    _assert_restored(suite, process, "missing activation relation")


def _server_identity_drift(suite: Suite, process: Any) -> None:
    baseline = suite.expect_status(200, "GET", READY_PATH, headers=suite.auth_headers("admin"))
    if baseline.get("result") != {"status": "ready", "scope": "store", "server_id": suite.server_id}:
        raise RuntimeError("deployment pin baseline did not use the expected Server identity")
    previous = suite.scalar("SELECT server_id FROM shin_server.server_identity WHERE singleton")
    if re.fullmatch(r"[0-9a-f]{32}", previous) is None or previous != suite.server_id:
        raise RuntimeError("Server identity fixture does not match the deployment pin")
    replacement = "0" * 31 + "1"
    if replacement == previous:
        replacement = "0" * 31 + "2"
    suite.psql(f"UPDATE shin_server.server_identity SET server_id = '{replacement}' WHERE singleton")
    try:
        ready = suite.wait_for_response(READY_PATH, 503, headers=suite.auth_headers("admin"))
        _assert_failure_code(ready, SERVER_IDENTITY_MISMATCH, "deployment pin drift")
        suite.expect_status(200, "GET", LIVE_PATH)
        _assert_server_running(suite, process)
    finally:
        suite.psql(f"UPDATE shin_server.server_identity SET server_id = '{previous}' WHERE singleton")
    _assert_restored(suite, process, "deployment pin drift")


def check_store_validation_drift(suite: Suite) -> None:
    """Exercise Store validation on a running, already healthy Server instance."""
    process = suite.server
    _assert_server_running(suite, process)
    _assert_restored(suite, process, "initial Store validation state")
    for domain, table in DOMAIN_PROBES:
        _probe_column_drift(suite, process, domain, table)
    _constraint_drift(suite, process)
    _preparation_scope_drift(suite, process)
    _preparation_relational_scope_drift(suite, process)
    _request_key_index_drift(suite, process)
    _workspace_request_key_drift(suite, process)
    _checkpoint_namespace_drift(suite, process)
    _publication_scope_drift(suite, process)
    _index_drift(suite, process)
    _multiple_domain_drift(suite, process)
    _missing_relation_drift(suite, process)
    _server_identity_drift(suite, process)
