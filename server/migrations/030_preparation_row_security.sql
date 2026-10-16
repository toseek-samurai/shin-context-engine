ALTER TABLE shin_server.preparations ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.preparations FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.preparation_retention ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.preparation_retention FORCE ROW LEVEL SECURITY;

CREATE POLICY preparations_ops ON shin_server.preparations USING
    (current_setting('shin.identity_ops',true)='1' OR current_setting('shin.preparation_ops',true)='1');
CREATE POLICY preparations_owner ON shin_server.preparations USING (
    principal_id=current_setting('shin.identity_principal',true)
    AND project_id=current_setting('shin.content_project',true)
    AND scope_tenant_id=current_setting('shin.identity_tenant',true)
    AND scope_workspace_id=current_setting('shin.content_workspace',true)
    AND EXISTS (SELECT 1 FROM shin_server.identity_workspace_owners w
        JOIN shin_server.identity_accounts a ON a.id=w.owner_id
        WHERE w.project_id=preparations.project_id AND w.workspace_id=preparations.scope_workspace_id
          AND w.tenant_id=preparations.scope_tenant_id AND a.principal_id=preparations.principal_id AND a.enabled)
);
CREATE POLICY preparations_legacy_read ON shin_server.preparations FOR SELECT USING (
    workspace_admission IS NULL AND current_setting('shin.preparation_legacy',true)='1'
    AND project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects',true),','))
);
CREATE POLICY preparations_source_read ON shin_server.preparations FOR SELECT USING (
    project_id=current_setting('shin.content_project',true)
    AND id::text=ANY(string_to_array(current_setting('shin.preparation_sources',true),','))
    AND EXISTS (SELECT 1 FROM shin_server.identity_workspace_owners w
        JOIN shin_server.identity_accounts a ON a.id=w.owner_id
        WHERE w.project_id=preparations.project_id AND w.workspace_id=current_setting('shin.content_workspace',true)
          AND w.tenant_id=current_setting('shin.identity_tenant',true)
          AND a.principal_id=current_setting('shin.identity_principal',true) AND a.enabled)
);

CREATE POLICY preparation_retention_ops ON shin_server.preparation_retention USING
    (current_setting('shin.identity_ops',true)='1' OR current_setting('shin.preparation_ops',true)='1');
CREATE POLICY preparation_retention_read ON shin_server.preparation_retention FOR SELECT USING (
    EXISTS (SELECT 1 FROM shin_server.preparations p WHERE p.id=preparation_retention.preparation_id)
);
CREATE POLICY preparation_retention_owner ON shin_server.preparation_retention USING (
    EXISTS (SELECT 1 FROM shin_server.preparations p WHERE p.id=preparation_retention.preparation_id
        AND p.principal_id=current_setting('shin.identity_principal',true)
        AND p.project_id=current_setting('shin.content_project',true)
        AND p.scope_tenant_id=current_setting('shin.identity_tenant',true)
        AND p.scope_workspace_id=current_setting('shin.content_workspace',true))
);
