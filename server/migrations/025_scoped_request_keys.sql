-- Preserve receipts and captured authority; only split uniqueness domains.
ALTER TABLE shin_server.preparations
    DROP CONSTRAINT preparations_principal_id_idempotency_key_key;
CREATE UNIQUE INDEX preparations_legacy_request_unique
    ON shin_server.preparations (principal_id, idempotency_key)
    WHERE workspace_admission IS NULL;
CREATE UNIQUE INDEX preparations_workspace_request_unique
    ON shin_server.preparations (principal_id, project_id,
        (COALESCE(workspace_admission #>> '{authorization,ownership,tenant_id}', '')),
        (COALESCE(workspace_admission #>> '{authorization,ownership,workspace_id}', '')),
        idempotency_key)
    WHERE workspace_admission IS NOT NULL;

ALTER TABLE shin_server.publication_proposals
    DROP CONSTRAINT publication_proposals_request_unique;
CREATE UNIQUE INDEX publication_proposals_legacy_request_unique
    ON shin_server.publication_proposals (submitted_by, idempotency_key)
    WHERE history_workspace_id IS NULL;
CREATE UNIQUE INDEX publication_proposals_workspace_request_unique
    ON shin_server.publication_proposals (submitted_by, project_id, history_workspace_id, idempotency_key)
    WHERE history_workspace_id IS NOT NULL;
