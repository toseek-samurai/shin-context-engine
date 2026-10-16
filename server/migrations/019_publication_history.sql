ALTER TABLE shin_server.publication_proposals
    ADD COLUMN history_epoch BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN base_source_route_id TEXT,
    ADD COLUMN base_workspace_id TEXT,
    ADD COLUMN base_source_host_id TEXT,
    ADD COLUMN base_profile_id TEXT,
    ADD COLUMN base_route_fingerprint TEXT;

-- Capture immutable base provenance before any historical transition can retire.
UPDATE shin_server.publication_proposals p SET
    base_source_route_id = b.source_route_id,
    base_workspace_id = b.workspace_id,
    base_source_host_id = b.source_host_id,
    base_profile_id = b.profile_id,
    base_route_fingerprint = b.route_fingerprint
FROM shin_server.publication_resolutions r
JOIN shin_server.publication_proposals b ON b.id = r.proposal_id
WHERE p.base_preparation_id IS NOT NULL
  AND r.project_id = p.project_id AND r.publication_id = p.publication_id
  AND r.decision = 'accept' AND r.revision = p.base_revision
  AND r.preparation_id = p.base_preparation_id;

ALTER TABLE shin_server.publication_proposals
    DROP CONSTRAINT publication_proposals_withdrawal_fk,
    ADD CONSTRAINT publication_proposals_epoch_check CHECK (history_epoch >= 0),
    ADD CONSTRAINT publication_proposals_base_origin_check CHECK (
        ((base_preparation_id IS NULL) = (base_source_route_id IS NULL)) AND
        ((base_preparation_id IS NULL) = (base_workspace_id IS NULL)) AND
        ((base_preparation_id IS NULL) = (base_source_host_id IS NULL)) AND
        ((base_preparation_id IS NULL) = (base_profile_id IS NULL)) AND
        ((base_preparation_id IS NULL) = (base_route_fingerprint IS NULL))
    ),
    ADD CONSTRAINT publication_proposals_base_route_check CHECK (base_source_route_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    ADD CONSTRAINT publication_proposals_base_workspace_check CHECK (base_workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    ADD CONSTRAINT publication_proposals_base_host_check CHECK (base_source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    ADD CONSTRAINT publication_proposals_base_profile_check CHECK (base_profile_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    ADD CONSTRAINT publication_proposals_base_fingerprint_check CHECK (base_route_fingerprint ~ '^[0-9a-f]{64}$');

CREATE TABLE shin_server.publication_history (
    singleton BOOLEAN NOT NULL DEFAULT true,
    history_epoch BIGINT NOT NULL DEFAULT 0,
    retired_proposals BIGINT NOT NULL DEFAULT 0,
    last_retention JSONB,
    CONSTRAINT publication_history_pkey PRIMARY KEY (singleton),
    CONSTRAINT publication_history_singleton_check CHECK (singleton),
    CONSTRAINT publication_history_epoch_check CHECK (history_epoch >= 0),
    CONSTRAINT publication_history_count_check CHECK (retired_proposals >= 0),
    CONSTRAINT publication_history_receipt_check CHECK (
        last_retention IS NULL OR
        (jsonb_typeof(last_retention) = 'object' AND octet_length(last_retention::text) <= 65536)
    )
);
INSERT INTO shin_server.publication_history(singleton) VALUES(true);
