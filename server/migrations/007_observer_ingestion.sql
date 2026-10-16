ALTER TABLE shin_server.workspaces
    ALTER COLUMN root DROP NOT NULL,
    ALTER COLUMN root_identity DROP NOT NULL,
    ADD COLUMN source_kind TEXT NOT NULL DEFAULT 'local' CHECK (source_kind IN ('local', 'observer')),
    ADD CONSTRAINT workspace_source_shape CHECK (
        (source_kind = 'local' AND root IS NOT NULL AND root_identity IS NOT NULL) OR
        (source_kind = 'observer' AND root IS NULL AND root_identity IS NULL)
    );

-- Schema 006 could only register local sources. Upgrade the response shape once,
-- preserving request identity, physical binding and all prior response fields.
UPDATE shin_server.workspace_requests SET response=response || '{"source_kind":"local"}'::jsonb;

CREATE TABLE shin_server.observer_streams (
    stream_id TEXT PRIMARY KEY CHECK (stream_id ~ '^[0-9a-f]{32}$'),
    project_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    observer_id TEXT NOT NULL CHECK (observer_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    source_host_id TEXT NOT NULL CHECK (source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    profile TEXT NOT NULL CHECK (profile ~ '^[a-z][a-z0-9-]{0,62}$'),
    binding_digest TEXT NOT NULL CHECK (binding_digest ~ '^[0-9a-f]{64}$'),
    enrollment_hash TEXT NOT NULL CHECK (enrollment_hash ~ '^[0-9a-f]{64}$'),
    revoked BOOLEAN NOT NULL DEFAULT false,
    accepted_sequence BIGINT NOT NULL DEFAULT 0 CHECK (accepted_sequence BETWEEN 0 AND 65536),
    observed_sequence BIGINT NOT NULL DEFAULT 0 CHECK (observed_sequence >= 0),
    retained_bytes BIGINT NOT NULL DEFAULT 0 CHECK (retained_bytes BETWEEN 0 AND 4194304),
    enrolled_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT,
    UNIQUE (project_id,workspace_id,stream_id)
);
CREATE UNIQUE INDEX observer_active_identity ON shin_server.observer_streams(project_id,workspace_id,observer_id) WHERE NOT revoked;
CREATE INDEX observer_owner_cursor ON shin_server.observer_streams(principal_id,stream_id);

CREATE TABLE shin_server.observer_submissions (
    stream_id TEXT NOT NULL REFERENCES shin_server.observer_streams(stream_id) ON DELETE RESTRICT,
    sequence BIGINT NOT NULL CHECK (sequence BETWEEN 1 AND 65536),
    submission_id TEXT NOT NULL UNIQUE CHECK (submission_id ~ '^[0-9a-f]{64}$'),
    submission_hash TEXT NOT NULL CHECK (submission_hash ~ '^[0-9a-f]{64}$'),
    payload_hash TEXT NOT NULL CHECK (payload_hash ~ '^[0-9a-f]{64}$'),
    submission JSONB NOT NULL CHECK (jsonb_typeof(submission) = 'object'),
    receipt JSONB NOT NULL CHECK (jsonb_typeof(receipt) = 'object'),
    retained_bytes INTEGER NOT NULL CHECK (retained_bytes BETWEEN 1 AND 12288),
    received_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (stream_id, sequence)
);

CREATE TABLE shin_server.observer_usage (
    singleton BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    server_id TEXT NOT NULL CHECK (server_id ~ '^[0-9a-f]{32}$'),
    streams BIGINT NOT NULL DEFAULT 0 CHECK (streams BETWEEN 0 AND 1024),
    submissions BIGINT NOT NULL DEFAULT 0 CHECK (submissions BETWEEN 0 AND 65536),
    retained_bytes BIGINT NOT NULL DEFAULT 0 CHECK (retained_bytes BETWEEN 0 AND 134217728)
);
INSERT INTO shin_server.observer_usage(server_id) VALUES (replace(gen_random_uuid()::text, '-', ''));

ALTER TABLE shin_server.audit_events
    ADD COLUMN observer_stream_id TEXT,
    ADD COLUMN observer_sequence BIGINT,
    ADD CONSTRAINT audit_observer_shape CHECK (
        (observer_stream_id IS NULL AND observer_sequence IS NULL) OR
        (observer_stream_id IS NOT NULL AND workspace_id IS NOT NULL AND (observer_sequence IS NULL OR observer_sequence > 0))
    ),
    ADD CONSTRAINT audit_observer_stream_fk FOREIGN KEY (project_id,workspace_id,observer_stream_id)
        REFERENCES shin_server.observer_streams(project_id,workspace_id,stream_id) ON DELETE RESTRICT,
    ADD CONSTRAINT audit_observer_submission_fk FOREIGN KEY (observer_stream_id,observer_sequence)
        REFERENCES shin_server.observer_submissions(stream_id,sequence) ON DELETE RESTRICT;
