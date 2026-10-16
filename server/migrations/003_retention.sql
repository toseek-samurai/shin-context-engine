ALTER TABLE shin_server.preparations
    ADD COLUMN artifact_namespace TEXT,
    ADD COLUMN artifact_key TEXT,
    ADD CONSTRAINT preparations_artifact_binding_check CHECK (
        (artifact_namespace IS NULL AND artifact_key IS NULL) OR
        (artifact_namespace IS NOT NULL AND artifact_key IS NOT NULL AND
         artifact_namespace ~ '^[0-9a-f]{64}$' AND artifact_key ~ '^[0-9a-f]{64}$')
    ),
    ADD CONSTRAINT preparations_artifact_key_unique UNIQUE (artifact_key);

CREATE TABLE shin_server.preparation_retention (
    preparation_id BIGINT PRIMARY KEY REFERENCES shin_server.preparations(id) ON DELETE RESTRICT,
    state TEXT NOT NULL,
    plan_id TEXT NOT NULL,
    inventory JSONB NOT NULL,
    requested_by TEXT NOT NULL,
    completed_by TEXT,
    requested_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    released_at TIMESTAMPTZ,
    CONSTRAINT preparation_retention_state_check CHECK (state IN ('intent', 'released')),
    CONSTRAINT preparation_retention_plan_id_check CHECK (plan_id ~ '^[0-9a-f]{64}$'),
    CONSTRAINT preparation_retention_inventory_check CHECK (
        jsonb_typeof(inventory) = 'object' AND
        octet_length(inventory::text) <= 32768 AND
        inventory ? 'entries' AND
        CASE WHEN jsonb_typeof(inventory -> 'entries') = 'array'
             THEN jsonb_array_length(inventory -> 'entries') <= 64 ELSE FALSE END
    ),
    CONSTRAINT preparation_retention_requested_by_check CHECK (requested_by ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT preparation_retention_completed_by_check CHECK (
        completed_by IS NULL OR completed_by ~ '^[a-z][a-z0-9-]{0,62}$'
    ),
    CONSTRAINT preparation_retention_state_shape_check CHECK (
        (state = 'intent' AND completed_by IS NULL AND released_at IS NULL) OR
        (state = 'released' AND completed_by IS NOT NULL AND released_at IS NOT NULL)
    )
);
