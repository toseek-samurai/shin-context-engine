ALTER TABLE shin_server.audit_events
    ADD COLUMN preparation_id BIGINT CHECK (preparation_id IS NULL OR preparation_id > 0);

CREATE TABLE shin_server.preparations (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    route_id TEXT NOT NULL CHECK (route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    idempotency_key TEXT NOT NULL CHECK (idempotency_key ~ '^[A-Za-z0-9._:-]{1,128}$'),
    request_hash TEXT NOT NULL CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    route_fingerprint TEXT NOT NULL CHECK (route_fingerprint ~ '^[0-9a-f]{64}$'),
    expected_snapshot_id TEXT NOT NULL CHECK (expected_snapshot_id = '' OR expected_snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'),
    state TEXT NOT NULL CHECK (state IN ('queued', 'running', 'prepared', 'failed', 'cancelled')),
    cancel_requested BOOLEAN NOT NULL DEFAULT FALSE,
    worker_id TEXT CHECK (worker_id IS NULL OR worker_id ~ '^[A-Za-z0-9._:-]{16,128}$'),
    lease_until TIMESTAMPTZ,
    artifact_snapshot_id TEXT CHECK (artifact_snapshot_id IS NULL OR artifact_snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'),
    artifact_bytes BIGINT CHECK (artifact_bytes IS NULL OR artifact_bytes >= 0),
    reserved_artifact_bytes BIGINT NOT NULL CHECK (reserved_artifact_bytes > 0 AND reserved_artifact_bytes <= 1073741824),
    reason_code TEXT CHECK (reason_code IS NULL OR reason_code ~ '^[a-z][a-z0-9-]{0,62}$'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (principal_id, idempotency_key),
    CHECK (
        (state = 'queued' AND NOT cancel_requested AND worker_id IS NULL AND lease_until IS NULL AND
         artifact_snapshot_id IS NULL AND artifact_bytes IS NULL AND reason_code IS NULL) OR
        (state = 'running' AND worker_id IS NOT NULL AND lease_until IS NOT NULL AND
         artifact_snapshot_id IS NULL AND artifact_bytes IS NULL AND reason_code IS NULL) OR
        (state = 'prepared' AND NOT cancel_requested AND worker_id IS NOT NULL AND lease_until IS NULL AND
         artifact_snapshot_id IS NOT NULL AND artifact_bytes IS NOT NULL AND
         artifact_bytes <= reserved_artifact_bytes AND reason_code IS NULL) OR
        (state = 'failed' AND NOT cancel_requested AND lease_until IS NULL AND
         artifact_snapshot_id IS NULL AND artifact_bytes IS NULL AND reason_code IS NOT NULL) OR
        (state = 'cancelled' AND cancel_requested AND lease_until IS NULL AND
         artifact_snapshot_id IS NULL AND artifact_bytes IS NULL AND reason_code IS NOT NULL)
    )
);

CREATE INDEX preparations_project_id_id_idx ON shin_server.preparations (project_id, id);
CREATE INDEX preparations_queue_idx ON shin_server.preparations (state, id);
CREATE INDEX preparations_lease_idx ON shin_server.preparations (state, lease_until, id);
