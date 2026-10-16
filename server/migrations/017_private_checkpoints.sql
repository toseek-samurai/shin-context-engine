CREATE SEQUENCE shin_server.checkpoint_revisions
    AS BIGINT
    START WITH 1
    INCREMENT BY 1
    MINVALUE 1
    MAXVALUE 9223372036854775807
    NO CYCLE
    OWNED BY NONE;

CREATE TABLE shin_server.checkpoint_slots (
    project_id TEXT NOT NULL,
    owner_id TEXT NOT NULL,
    slot_id TEXT NOT NULL,
    state TEXT NOT NULL,
    revision BIGINT NOT NULL,
    task_id TEXT NOT NULL,
    thread_id TEXT NOT NULL,
    provenance JSONB NOT NULL,
    checkpoint_text TEXT,
    checkpoint_sha256 TEXT,
    checkpoint_id TEXT,
    captured_at BIGINT,
    ttl_seconds BIGINT,
    snapshot_id TEXT,
    last_operation TEXT NOT NULL,
    last_request_id TEXT NOT NULL,
    last_request_hash TEXT NOT NULL,
    last_expected_revision BIGINT NOT NULL,
    last_request_deadline BIGINT NOT NULL,
    retry_until BIGINT NOT NULL,
    updated_at BIGINT NOT NULL,
    CONSTRAINT checkpoint_slots_pkey PRIMARY KEY (project_id, owner_id, slot_id),
    CONSTRAINT checkpoint_slots_revision_unique UNIQUE (revision),
    CONSTRAINT checkpoint_slots_project_fk FOREIGN KEY (project_id)
        REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    CONSTRAINT checkpoint_slots_owner_id_check CHECK (owner_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT checkpoint_slots_slot_id_check CHECK (slot_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT checkpoint_slots_state_check CHECK (state = 'active' OR state = 'cleared'),
    CONSTRAINT checkpoint_slots_revision_check CHECK (revision > 0),
    CONSTRAINT checkpoint_slots_binding_check CHECK (
        task_id ~ '^[!-~]{1,128}$' AND thread_id ~ '^[!-~]{1,128}$'
    ),
    CONSTRAINT checkpoint_slots_provenance_check CHECK (
        jsonb_typeof(provenance) = 'object' AND octet_length(provenance::text) <= 4096
    ),
    CONSTRAINT checkpoint_slots_payload_check CHECK (
        (state = 'active' AND checkpoint_text IS NOT NULL AND octet_length(checkpoint_text) BETWEEN 1 AND 262144 AND
            checkpoint_sha256 IS NOT NULL AND checkpoint_id IS NOT NULL AND captured_at IS NOT NULL AND
            ttl_seconds IS NOT NULL AND snapshot_id IS NOT NULL) OR
        (state = 'cleared' AND checkpoint_text IS NULL AND checkpoint_sha256 IS NULL AND checkpoint_id IS NULL AND
            captured_at IS NULL AND ttl_seconds IS NULL AND snapshot_id IS NULL)
    ),
    CONSTRAINT checkpoint_slots_checkpoint_fields_check CHECK (
        (checkpoint_sha256 IS NULL OR checkpoint_sha256 ~ '^[0-9a-f]{64}$') AND
        (checkpoint_id IS NULL OR checkpoint_id ~ '^fnv1a64:[0-9a-f]{16}$') AND
        (captured_at IS NULL OR captured_at BETWEEN 0 AND 253402300799) AND
        (ttl_seconds IS NULL OR ttl_seconds BETWEEN 1 AND 604800) AND
        (snapshot_id IS NULL OR snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$')
    ),
    CONSTRAINT checkpoint_slots_receipt_check CHECK (
        (last_operation = 'save' OR last_operation = 'clear') AND
        last_request_id ~ '^[0-9a-f]{32}$' AND last_request_hash ~ '^[0-9a-f]{64}$' AND
        last_expected_revision >= 0 AND last_request_deadline BETWEEN 0 AND 253402300799 AND
        retry_until BETWEEN last_request_deadline AND 253402300799
    ),
    CONSTRAINT checkpoint_slots_updated_at_check CHECK (updated_at BETWEEN 0 AND 253402300799)
);

CREATE INDEX checkpoint_slots_prune_idx
    ON shin_server.checkpoint_slots(project_id, owner_id, state, retry_until);

CREATE TABLE shin_server.checkpoint_lifecycle (
    singleton BOOLEAN NOT NULL DEFAULT true,
    reject_deadlines_through BIGINT NOT NULL DEFAULT 0,
    CONSTRAINT checkpoint_lifecycle_pkey PRIMARY KEY (singleton),
    CONSTRAINT checkpoint_lifecycle_singleton_check CHECK (singleton),
    CONSTRAINT checkpoint_lifecycle_deadline_floor_check CHECK (
        reject_deadlines_through BETWEEN 0 AND 253402300799
    )
);

INSERT INTO shin_server.checkpoint_lifecycle(singleton, reject_deadlines_through) VALUES (true, 0);
