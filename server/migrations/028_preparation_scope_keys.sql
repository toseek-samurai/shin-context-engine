-- Derive only explicitly captured coordinates; NULL legacy jobs stay unscoped.
ALTER TABLE shin_server.preparations
    ADD COLUMN scope_tenant_id TEXT GENERATED ALWAYS AS
        (workspace_admission #>> '{authorization,ownership,tenant_id}') STORED,
    ADD COLUMN scope_workspace_id TEXT GENERATED ALWAYS AS
        (workspace_admission #>> '{authorization,ownership,workspace_id}') STORED,
    ADD CONSTRAINT preparation_scope_identity_check CHECK (
        workspace_admission IS NULL OR ((
            scope_tenant_id IS NOT NULL AND scope_workspace_id IS NOT NULL AND
            workspace_admission #>> '{authorization,principal_id}' = principal_id AND
            workspace_admission #>> '{authorization,ownership,project_id}' = project_id
        ) IS TRUE)
    ),
    ADD CONSTRAINT preparation_scope_project_fk FOREIGN KEY (scope_tenant_id, project_id)
        REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT,
    ADD CONSTRAINT preparation_scope_workspace_fk FOREIGN KEY (project_id, scope_workspace_id)
        REFERENCES shin_server.identity_workspace_owners(project_id, workspace_id) ON DELETE RESTRICT;

DROP INDEX shin_server.preparations_workspace_request_unique;
CREATE UNIQUE INDEX preparations_workspace_request_unique
    ON shin_server.preparations (principal_id, project_id, scope_tenant_id, scope_workspace_id, idempotency_key)
    WHERE workspace_admission IS NOT NULL;
CREATE INDEX preparations_workspace_cursor_idx
    ON shin_server.preparations (principal_id, project_id, scope_tenant_id, scope_workspace_id, id);
