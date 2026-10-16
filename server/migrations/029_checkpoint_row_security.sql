ALTER TABLE shin_server.checkpoint_slots ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.checkpoint_slots FORCE ROW LEVEL SECURITY;

CREATE POLICY checkpoint_slots_ops ON shin_server.checkpoint_slots USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');

CREATE POLICY checkpoint_slots_scope ON shin_server.checkpoint_slots USING (
    owner_id = current_setting('shin.identity_principal', true)
    AND project_id = current_setting('shin.checkpoint_project', true)
    AND (
        (scope_workspace_id IS NULL AND current_setting('shin.checkpoint_legacy', true) = '1')
        OR (
            scope_workspace_id = current_setting('shin.checkpoint_workspace', true)
            AND EXISTS (
                SELECT 1 FROM shin_server.identity_workspace_owners w
                JOIN shin_server.identity_accounts a ON a.id = w.owner_id
                WHERE w.project_id = checkpoint_slots.project_id
                  AND w.workspace_id = checkpoint_slots.scope_workspace_id
                  AND w.tenant_id = current_setting('shin.identity_tenant', true)
                  AND a.principal_id = checkpoint_slots.owner_id AND a.enabled
            )
        )
    )
);
