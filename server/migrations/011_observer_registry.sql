CREATE TABLE shin_server.server_identity (
    singleton BOOLEAN PRIMARY KEY CHECK (singleton),
    server_id TEXT NOT NULL CHECK (server_id ~ '^[0-9a-f]{32}$')
);
INSERT INTO shin_server.server_identity(singleton,server_id)
    SELECT singleton,server_id FROM shin_server.observer_usage;
ALTER TABLE shin_server.observer_usage DROP COLUMN server_id,
    ADD COLUMN tombstones BIGINT NOT NULL DEFAULT 0 CHECK (tombstones BETWEEN 0 AND 4096);

CREATE TABLE shin_server.observer_tombstones (
    stream_id TEXT PRIMARY KEY CHECK (stream_id ~ '^[0-9a-f]{32}$'),
    project_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    enrollment_hash TEXT NOT NULL CHECK (enrollment_hash ~ '^[0-9a-f]{64}$'),
    retirement JSONB NOT NULL CHECK (jsonb_typeof(retirement) = 'object' AND octet_length(retirement::text) <= 8192),
    retired_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT
);
CREATE INDEX observer_tombstone_project ON shin_server.observer_tombstones(project_id,stream_id);

-- Audit references are historical identity, not a payload-retention promise.
ALTER TABLE shin_server.audit_events DROP CONSTRAINT audit_observer_stream_fk;
