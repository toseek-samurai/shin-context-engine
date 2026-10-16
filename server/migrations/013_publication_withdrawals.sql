CREATE TABLE shin_server.publication_withdrawals (
    id BIGINT GENERATED ALWAYS AS IDENTITY,
    project_id TEXT NOT NULL,
    publication_id TEXT NOT NULL,
    expected_revision BIGINT NOT NULL,
    revision BIGINT NOT NULL,
    previous_proposal_id BIGINT NOT NULL,
    previous_preparation_id BIGINT NOT NULL,
    previous_decision TEXT NOT NULL DEFAULT 'accept',
    withdrawn_by TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT publication_withdrawals_pkey PRIMARY KEY (id),
    CONSTRAINT publication_withdrawals_previous_unique UNIQUE (previous_proposal_id),
    CONSTRAINT publication_withdrawals_request_unique UNIQUE (project_id, publication_id, expected_revision),
    CONSTRAINT publication_withdrawals_revision_unique UNIQUE (project_id, publication_id, revision),
    CONSTRAINT publication_withdrawals_base_unique UNIQUE (project_id, publication_id, revision, id),
    CONSTRAINT publication_withdrawals_resolution_fk FOREIGN KEY
        (previous_proposal_id, project_id, publication_id, previous_decision, expected_revision, previous_preparation_id)
        REFERENCES shin_server.publication_resolutions
        (proposal_id, project_id, publication_id, decision, revision, preparation_id) ON DELETE RESTRICT,
    CONSTRAINT publication_withdrawals_id_check CHECK (id > 0),
    CONSTRAINT publication_withdrawals_expected_check CHECK (
        expected_revision > 0 AND expected_revision < 9223372036854775807
    ),
    CONSTRAINT publication_withdrawals_revision_check CHECK (revision > 0 AND revision - 1 = expected_revision),
    CONSTRAINT publication_withdrawals_decision_check CHECK (previous_decision = 'accept'),
    CONSTRAINT publication_withdrawals_author_check CHECK (withdrawn_by ~ '^[a-z][a-z0-9-]{0,62}$')
);

ALTER TABLE shin_server.publication_proposals
    ADD COLUMN base_withdrawal_id BIGINT,
    DROP CONSTRAINT publication_proposals_base_check,
    ADD CONSTRAINT publication_proposals_base_check CHECK (
        (base_revision = 0 AND base_preparation_id IS NULL AND base_withdrawal_id IS NULL) OR
        (base_revision > 0 AND base_preparation_id IS NOT NULL AND base_preparation_id > 0 AND base_withdrawal_id IS NULL) OR
        (base_revision > 0 AND base_preparation_id IS NULL AND base_withdrawal_id IS NOT NULL AND base_withdrawal_id > 0)
    ),
    ADD CONSTRAINT publication_proposals_withdrawal_fk FOREIGN KEY (project_id, publication_id, base_revision, base_withdrawal_id)
        REFERENCES shin_server.publication_withdrawals(project_id, publication_id, revision, id) ON DELETE RESTRICT;
CREATE INDEX publication_proposals_withdrawal_idx ON shin_server.publication_proposals(base_withdrawal_id);
