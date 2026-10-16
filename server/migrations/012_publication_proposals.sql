ALTER TABLE shin_server.preparations
    ADD CONSTRAINT preparations_publication_identity_unique UNIQUE (project_id, id);

CREATE TABLE shin_server.publication_proposals (
    id BIGINT GENERATED ALWAYS AS IDENTITY,
    project_id TEXT NOT NULL,
    publication_id TEXT NOT NULL,
    preparation_id BIGINT NOT NULL,
    base_revision BIGINT NOT NULL,
    base_preparation_id BIGINT,
    submitted_by TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    request_hash TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    source_route_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    source_host_id TEXT NOT NULL,
    profile_id TEXT NOT NULL,
    route_fingerprint TEXT NOT NULL,
    CONSTRAINT publication_proposals_pkey PRIMARY KEY (id),
    CONSTRAINT publication_proposals_project_identity_unique UNIQUE (project_id, id),
    CONSTRAINT publication_proposals_identity_unique UNIQUE (project_id, publication_id, id, preparation_id),
    CONSTRAINT publication_proposals_request_unique UNIQUE (submitted_by, idempotency_key),
    CONSTRAINT publication_proposals_project_fk FOREIGN KEY (project_id)
        REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    CONSTRAINT publication_proposals_preparation_fk FOREIGN KEY (project_id, preparation_id)
        REFERENCES shin_server.preparations(project_id, id) ON DELETE RESTRICT,
    CONSTRAINT publication_proposals_base_fk FOREIGN KEY (project_id, base_preparation_id)
        REFERENCES shin_server.preparations(project_id, id) ON DELETE RESTRICT,
    CONSTRAINT publication_proposals_source_fk FOREIGN KEY (project_id, source_route_id, preparation_id)
        REFERENCES shin_server.preparations(project_id, route_id, id) ON DELETE RESTRICT,
    CONSTRAINT publication_proposals_workspace_fk FOREIGN KEY (project_id, workspace_id)
        REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT,
    CONSTRAINT publication_proposals_id_check CHECK (id > 0),
    CONSTRAINT publication_proposals_publication_check CHECK (publication_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_preparation_check CHECK (preparation_id > 0),
    CONSTRAINT publication_proposals_base_check CHECK (
        (base_revision = 0 AND base_preparation_id IS NULL) OR
        (base_revision > 0 AND base_preparation_id IS NOT NULL AND base_preparation_id > 0)
    ),
    CONSTRAINT publication_proposals_author_check CHECK (submitted_by ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_key_check CHECK (idempotency_key ~ '^[A-Za-z0-9._:-]{1,128}$'),
    CONSTRAINT publication_proposals_hash_check CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    CONSTRAINT publication_proposals_source_route_check CHECK (source_route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_workspace_check CHECK (workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_source_host_check CHECK (source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_profile_check CHECK (profile_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    CONSTRAINT publication_proposals_fingerprint_check CHECK (route_fingerprint ~ '^[0-9a-f]{64}$')
);
CREATE INDEX publication_proposals_page_idx ON shin_server.publication_proposals(project_id, publication_id, id);
CREATE INDEX publication_proposals_preparation_idx ON shin_server.publication_proposals(preparation_id);
CREATE INDEX publication_proposals_base_idx ON shin_server.publication_proposals(base_preparation_id);

CREATE TABLE shin_server.publication_resolutions (
    proposal_id BIGINT NOT NULL,
    project_id TEXT NOT NULL,
    publication_id TEXT NOT NULL,
    decision TEXT NOT NULL,
    expected_revision BIGINT NOT NULL,
    revision BIGINT NOT NULL,
    preparation_id BIGINT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    resolved_by TEXT NOT NULL,
    CONSTRAINT publication_resolutions_pkey PRIMARY KEY (proposal_id),
    CONSTRAINT publication_resolutions_head_unique UNIQUE
        (proposal_id, project_id, publication_id, decision, revision, preparation_id),
    CONSTRAINT publication_resolutions_proposal_fk FOREIGN KEY (project_id, publication_id, proposal_id, preparation_id)
        REFERENCES shin_server.publication_proposals(project_id, publication_id, id, preparation_id) ON DELETE RESTRICT,
    CONSTRAINT publication_resolutions_decision_check CHECK (
        decision = 'accept' OR decision = 'reject' OR decision = 'request-reprepare'
    ),
    CONSTRAINT publication_resolutions_expected_check CHECK (
        expected_revision >= 0 AND expected_revision < 9223372036854775807
    ),
    CONSTRAINT publication_resolutions_revision_check CHECK (
        (decision = 'accept' AND revision > 0 AND revision - 1 = expected_revision) OR
        (decision <> 'accept' AND revision = expected_revision)
    ),
    CONSTRAINT publication_resolutions_author_check CHECK (resolved_by ~ '^[a-z][a-z0-9-]{0,62}$')
);
CREATE UNIQUE INDEX publication_resolutions_accepted_idx
    ON shin_server.publication_resolutions(project_id, publication_id, revision) WHERE decision = 'accept';

CREATE TABLE shin_server.publication_heads (
    project_id TEXT NOT NULL,
    publication_id TEXT NOT NULL,
    revision BIGINT NOT NULL,
    preparation_id BIGINT NOT NULL,
    snapshot_id TEXT NOT NULL,
    route_fingerprint TEXT NOT NULL,
    proposal_id BIGINT NOT NULL,
    decision TEXT NOT NULL DEFAULT 'accept',
    CONSTRAINT publication_heads_pkey PRIMARY KEY (project_id, publication_id),
    CONSTRAINT publication_heads_preparation_fk FOREIGN KEY (project_id, preparation_id)
        REFERENCES shin_server.preparations(project_id, id) ON DELETE RESTRICT,
    CONSTRAINT publication_heads_resolution_fk FOREIGN KEY
        (proposal_id, project_id, publication_id, decision, revision, preparation_id)
        REFERENCES shin_server.publication_resolutions
        (proposal_id, project_id, publication_id, decision, revision, preparation_id) ON DELETE RESTRICT,
    CONSTRAINT publication_heads_revision_check CHECK (revision > 0),
    CONSTRAINT publication_heads_snapshot_check CHECK (snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'),
    CONSTRAINT publication_heads_fingerprint_check CHECK (route_fingerprint ~ '^[0-9a-f]{64}$'),
    CONSTRAINT publication_heads_decision_check CHECK (decision = 'accept')
);
CREATE INDEX publication_heads_preparation_idx ON shin_server.publication_heads(preparation_id);

ALTER TABLE shin_server.audit_events
    ADD COLUMN publication_proposal_id BIGINT,
    ADD CONSTRAINT audit_publication_proposal_fk FOREIGN KEY (project_id, publication_proposal_id)
        REFERENCES shin_server.publication_proposals(project_id, id) ON DELETE RESTRICT;
