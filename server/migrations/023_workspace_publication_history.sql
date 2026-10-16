-- No ownership inference for existing publications or retired history.
CREATE TABLE shin_server.workspace_publication_history (
    project_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    history_epoch BIGINT NOT NULL DEFAULT 0 CHECK (history_epoch >= 0),
    retired_proposals BIGINT NOT NULL DEFAULT 0 CHECK (retired_proposals >= 0),
    last_retention JSONB CHECK (last_retention IS NULL OR
        (jsonb_typeof(last_retention) = 'object' AND octet_length(last_retention::text) <= 65536)),
    PRIMARY KEY (project_id, workspace_id),
    FOREIGN KEY (project_id, workspace_id) REFERENCES
        shin_server.identity_workspace_owners(project_id, workspace_id) ON DELETE RESTRICT
);
ALTER TABLE shin_server.publication_proposals ADD COLUMN history_workspace_id TEXT;
ALTER TABLE shin_server.publication_proposals ADD CONSTRAINT publication_history_workspace_check
    CHECK (history_workspace_id IS NULL OR history_workspace_id = workspace_id);
ALTER TABLE shin_server.publication_proposals ADD CONSTRAINT publication_history_workspace_fk
    FOREIGN KEY (project_id, history_workspace_id) REFERENCES
        shin_server.workspace_publication_history(project_id, workspace_id) ON DELETE RESTRICT;
