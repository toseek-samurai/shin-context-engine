-- Preserve historical memberships/visibility, but never use them as sharing grants.
ALTER POLICY identity_tenants_read ON shin_server.identity_tenants USING
    (owner_id = current_setting('shin.identity_account', true) AND EXISTS
        (SELECT 1 FROM shin_server.identity_memberships m WHERE m.tenant_id = identity_tenants.id
         AND m.account_id = current_setting('shin.identity_account', true) AND m.enabled AND m.role = 'owner'));
ALTER POLICY identity_memberships_read ON shin_server.identity_memberships USING
    (account_id = current_setting('shin.identity_account', true));
ALTER POLICY identity_memberships_manage ON shin_server.identity_memberships USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');
ALTER POLICY identity_workspace_owners_read ON shin_server.identity_workspace_owners USING
    (tenant_id = current_setting('shin.identity_tenant', true)
     AND owner_id = current_setting('shin.identity_account', true)
     AND EXISTS (SELECT 1 FROM shin_server.identity_tenants t WHERE t.id = identity_workspace_owners.tenant_id
         AND t.owner_id = current_setting('shin.identity_account', true)));
ALTER POLICY identity_project_owners_read ON shin_server.identity_project_owners USING
    ((tenant_id = current_setting('shin.identity_tenant', true)
      AND EXISTS (SELECT 1 FROM shin_server.identity_tenants t WHERE t.id = identity_project_owners.tenant_id
          AND t.owner_id = current_setting('shin.identity_account', true)))
     OR project_id = ANY(string_to_array(current_setting('shin.identity_legacy_projects', true), ',')));
