CREATE TABLE shin_server.workspaces (
    project_id TEXT NOT NULL REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    id TEXT NOT NULL CHECK (id ~ '^[a-z][a-z0-9-]{0,62}$'),
    source_host_id TEXT NOT NULL CHECK (source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    display_name TEXT NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 120),
    root TEXT NOT NULL CHECK (octet_length(root) BETWEEN 1 AND 4096),
    root_identity TEXT NOT NULL CHECK (octet_length(root_identity) BETWEEN 1 AND 4096),
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision = 1),
    PRIMARY KEY (project_id, id),
    UNIQUE (source_host_id, root),
    UNIQUE (source_host_id, root_identity)
);

-- Explicit migration binds pre-Observer local roots to the reviewed source host.
INSERT INTO shin_server.workspaces
    (project_id, id, source_host_id, display_name, root, root_identity, created_at, revision)
SELECT id, 'primary', current_setting('shin_server.migration_source_host_id'),
       display_name, root, root_identity, created_at, revision
FROM shin_server.projects;
ALTER TABLE shin_server.projects DROP COLUMN root, DROP COLUMN root_identity;

CREATE TABLE shin_server.workspace_requests (
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    key TEXT NOT NULL CHECK (key ~ '^[A-Za-z0-9._:-]{1,128}$'),
    request_hash TEXT NOT NULL CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    project_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    response JSONB NOT NULL CHECK (jsonb_typeof(response) = 'object'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (principal_id, key),
    FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT
);

ALTER TABLE shin_server.audit_events ADD COLUMN workspace_id TEXT;
ALTER TABLE shin_server.audit_events ADD CONSTRAINT audit_workspace_fk
    FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT;
