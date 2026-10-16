-- Explicit bindings only. Existing projects/workspaces remain unassigned.
CREATE TABLE shin_server.identity_project_owners (
    project_id TEXT PRIMARY KEY REFERENCES shin_server.projects(id) ON DELETE RESTRICT,
    tenant_id TEXT NOT NULL REFERENCES shin_server.identity_tenants(id) ON DELETE RESTRICT,
    UNIQUE (tenant_id, project_id)
);
CREATE TABLE shin_server.identity_workspace_owners (
    tenant_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    workspace_id TEXT NOT NULL,
    owner_id TEXT NOT NULL,
    visibility TEXT NOT NULL DEFAULT 'private' CHECK (visibility IN ('private','shared')),
    PRIMARY KEY (project_id, workspace_id),
    FOREIGN KEY (tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT,
    FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT,
    FOREIGN KEY (tenant_id, owner_id) REFERENCES shin_server.identity_memberships(tenant_id, account_id) ON DELETE RESTRICT
);
CREATE TABLE shin_server.identity_ownership_audit (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_id TEXT NOT NULL CHECK (actor_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    tenant_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    workspace_id TEXT,
    action TEXT NOT NULL CHECK (action IN ('project.bound','workspace.bound')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    FOREIGN KEY (tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT
);
ALTER TABLE shin_server.identity_project_owners ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_project_owners FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_workspace_owners ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_workspace_owners FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_ownership_audit ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_ownership_audit FORCE ROW LEVEL SECURITY;

CREATE POLICY identity_project_owners_read ON shin_server.identity_project_owners FOR SELECT USING
    (tenant_id = current_setting('shin.identity_tenant', true)
     OR project_id = ANY(string_to_array(current_setting('shin.identity_legacy_projects', true), ',')));
CREATE POLICY identity_project_owners_ops ON shin_server.identity_project_owners FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');
CREATE POLICY identity_workspace_owners_read ON shin_server.identity_workspace_owners FOR SELECT USING
    (tenant_id = current_setting('shin.identity_tenant', true)
     AND (owner_id = current_setting('shin.identity_account', true) OR visibility = 'shared'));
CREATE POLICY identity_workspace_owners_ops ON shin_server.identity_workspace_owners FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');
CREATE POLICY identity_ownership_audit_read ON shin_server.identity_ownership_audit FOR SELECT USING
    (current_setting('shin.identity_ops', true) = '1');
CREATE POLICY identity_ownership_audit_write ON shin_server.identity_ownership_audit FOR INSERT WITH CHECK
    (current_setting('shin.identity_ops', true) = '1' AND actor_id = current_setting('shin.identity_principal', true));
