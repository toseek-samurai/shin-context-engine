CREATE SCHEMA IF NOT EXISTS shin_server;

CREATE TABLE shin_server.schema_migrations (
    version INTEGER PRIMARY KEY,
    checksum TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE shin_server.projects (
    id TEXT PRIMARY KEY CHECK (id ~ '^[a-z][a-z0-9-]{0,62}$'),
    display_name TEXT NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 120),
    root TEXT NOT NULL UNIQUE CHECK (octet_length(root) BETWEEN 1 AND 4096),
    root_identity TEXT NOT NULL CHECK (octet_length(root_identity) BETWEEN 1 AND 4096),
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision = 1)
);

CREATE TABLE shin_server.audit_events (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    action TEXT NOT NULL,
    project_id TEXT NOT NULL REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX audit_events_project_id_id_idx ON shin_server.audit_events (project_id, id);

CREATE TABLE shin_server.idempotency_requests (
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    key TEXT NOT NULL CHECK (key ~ '^[A-Za-z0-9._:-]{1,128}$'),
    request_hash TEXT NOT NULL CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    project JSONB NOT NULL CHECK (jsonb_typeof(project) = 'object'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (principal_id, key)
);
