ALTER TABLE shin_server.preparations
    ADD COLUMN expected_route_revision BIGINT NOT NULL DEFAULT 0,
    ADD CONSTRAINT preparations_base_revision_check CHECK (expected_route_revision >= 0),
    ADD CONSTRAINT preparations_route_identity_unique UNIQUE (project_id, route_id, id);

CREATE TABLE shin_server.active_routes (
    project_id TEXT NOT NULL REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    route_id TEXT NOT NULL CHECK (route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    revision BIGINT NOT NULL CHECK (revision > 0),
    preparation_id BIGINT,
    snapshot_id TEXT,
    route_fingerprint TEXT,
    PRIMARY KEY (project_id, route_id),
    FOREIGN KEY (project_id, route_id, preparation_id)
        REFERENCES shin_server.preparations(project_id, route_id, id) ON DELETE RESTRICT,
    CONSTRAINT active_routes_shape_check CHECK (
        (preparation_id IS NULL AND snapshot_id IS NULL AND route_fingerprint IS NULL) OR
        (preparation_id IS NOT NULL AND snapshot_id IS NOT NULL AND route_fingerprint IS NOT NULL AND
         snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$' AND route_fingerprint ~ '^[0-9a-f]{64}$')
    )
);
CREATE INDEX active_routes_preparation_idx ON shin_server.active_routes(preparation_id);

CREATE TABLE shin_server.activation_receipts (
    project_id TEXT NOT NULL REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    route_id TEXT NOT NULL CHECK (route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    previous_revision BIGINT NOT NULL CHECK (previous_revision >= 0 AND previous_revision < 9223372036854775807),
    revision BIGINT NOT NULL,
    preparation_id BIGINT,
    snapshot_id TEXT,
    principal_id TEXT NOT NULL CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (project_id, route_id, previous_revision),
    FOREIGN KEY (project_id, route_id, preparation_id)
        REFERENCES shin_server.preparations(project_id, route_id, id) ON DELETE RESTRICT,
    CONSTRAINT activation_receipts_revision_check CHECK (revision = previous_revision + 1),
    CONSTRAINT activation_receipts_shape_check CHECK (
        (preparation_id IS NULL AND snapshot_id IS NULL) OR
        (preparation_id IS NOT NULL AND snapshot_id IS NOT NULL AND snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$')
    )
);
