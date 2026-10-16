-- Evaluate live directory authority once per statement, not once per content row.
-- Invoker rights preserve directory RLS; this function never acquires maintenance authority.
CREATE FUNCTION shin_server.current_workspace_allowed() RETURNS boolean
LANGUAGE sql STABLE SECURITY INVOKER SET search_path = pg_catalog AS $body$
SELECT EXISTS (
 SELECT 1 FROM shin_server.identity_workspace_owners w
 JOIN shin_server.identity_accounts a ON a.id=w.owner_id
 WHERE w.project_id=current_setting('shin.content_project',true)
 AND w.workspace_id=current_setting('shin.content_workspace',true)
 AND w.tenant_id=current_setting('shin.identity_tenant',true)
 AND a.principal_id=current_setting('shin.identity_principal',true) AND a.enabled
)
$body$;
REVOKE ALL ON FUNCTION shin_server.current_workspace_allowed() FROM PUBLIC;

ALTER POLICY preparations_owner ON shin_server.preparations USING (
 principal_id=current_setting('shin.identity_principal',true)
 AND project_id=current_setting('shin.content_project',true)
 AND scope_tenant_id=current_setting('shin.identity_tenant',true)
 AND scope_workspace_id=current_setting('shin.content_workspace',true)
 AND (SELECT shin_server.current_workspace_allowed())
);
ALTER POLICY preparations_source_read ON shin_server.preparations USING (
 project_id=current_setting('shin.content_project',true)
 AND id::text=ANY(string_to_array(current_setting('shin.preparation_sources',true),','))
 AND (SELECT shin_server.current_workspace_allowed())
);

ALTER TABLE shin_server.active_routes ADD COLUMN scope_workspace_id text,
 ADD CONSTRAINT active_routes_workspace_fk FOREIGN KEY(project_id,scope_workspace_id)
 REFERENCES shin_server.identity_workspace_owners(project_id,workspace_id) ON DELETE RESTRICT;
ALTER TABLE shin_server.activation_receipts ADD COLUMN scope_workspace_id text,
 ADD CONSTRAINT activation_receipts_workspace_fk FOREIGN KEY(project_id,scope_workspace_id)
 REFERENCES shin_server.identity_workspace_owners(project_id,workspace_id) ON DELETE RESTRICT;
ALTER TABLE shin_server.active_routes ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.active_routes FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.activation_receipts ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.activation_receipts FORCE ROW LEVEL SECURITY;
CREATE POLICY active_routes_ops ON shin_server.active_routes USING (current_setting('shin.identity_ops',true)='1');
CREATE POLICY activation_receipts_ops ON shin_server.activation_receipts USING (current_setting('shin.identity_ops',true)='1');
CREATE POLICY active_routes_references ON shin_server.active_routes FOR SELECT USING (
 current_setting('shin.route_refs',true)='1' OR current_setting('shin.preparation_ops',true)='1'
);
CREATE POLICY activation_receipts_references ON shin_server.activation_receipts FOR SELECT USING (current_setting('shin.route_refs',true)='1');
CREATE POLICY active_routes_legacy ON shin_server.active_routes FOR SELECT USING (
 scope_workspace_id IS NULL AND current_setting('shin.preparation_legacy',true)='1'
 AND project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects',true),','))
);
CREATE POLICY activation_receipts_legacy ON shin_server.activation_receipts FOR SELECT USING (
 scope_workspace_id IS NULL AND current_setting('shin.preparation_legacy',true)='1'
 AND project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects',true),','))
);
CREATE POLICY active_routes_owner ON shin_server.active_routes USING (
 project_id=current_setting('shin.content_project',true)
 AND ((scope_workspace_id IS NULL AND route_id=current_setting('shin.content_route',true))
 OR scope_workspace_id=current_setting('shin.content_workspace',true))
 AND (SELECT shin_server.current_workspace_allowed())
) WITH CHECK (
 project_id=current_setting('shin.content_project',true)
 AND route_id=current_setting('shin.content_route',true)
 AND scope_workspace_id=current_setting('shin.content_workspace',true)
 AND (SELECT shin_server.current_workspace_allowed())
 AND (preparation_id IS NULL OR EXISTS (SELECT 1 FROM shin_server.preparations p
 WHERE p.id=active_routes.preparation_id AND p.project_id=active_routes.project_id
 AND p.scope_workspace_id=active_routes.scope_workspace_id
 AND p.principal_id=current_setting('shin.identity_principal',true)))
);
CREATE POLICY activation_receipts_owner ON shin_server.activation_receipts USING (
 project_id=current_setting('shin.content_project',true)
 AND ((scope_workspace_id IS NULL AND route_id=current_setting('shin.content_route',true))
 OR scope_workspace_id=current_setting('shin.content_workspace',true))
 AND (SELECT shin_server.current_workspace_allowed())
) WITH CHECK (
 project_id=current_setting('shin.content_project',true)
 AND route_id=current_setting('shin.content_route',true)
 AND principal_id=current_setting('shin.identity_principal',true)
 AND scope_workspace_id=current_setting('shin.content_workspace',true)
 AND (SELECT shin_server.current_workspace_allowed())
 AND (preparation_id IS NULL OR EXISTS (SELECT 1 FROM shin_server.preparations p
 WHERE p.id=activation_receipts.preparation_id AND p.project_id=activation_receipts.project_id
 AND p.scope_workspace_id=activation_receipts.scope_workspace_id
 AND p.principal_id=current_setting('shin.identity_principal',true)))
);

-- Histories are service-owned until an explicitly scoped history API exists.
-- Append-only audit permission does not confer read/delete or journal authority.
ALTER TABLE shin_server.audit_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.audit_events FORCE ROW LEVEL SECURITY;
CREATE POLICY audit_events_ops ON shin_server.audit_events USING (current_setting('shin.audit_ops',true)='1');
CREATE POLICY audit_events_append ON shin_server.audit_events FOR INSERT WITH CHECK (current_setting('shin.audit_append',true)='1');
CREATE POLICY audit_events_legacy ON shin_server.audit_events FOR SELECT USING (
 project_id=ANY(string_to_array(current_setting('shin.audit_projects',true),','))
);
ALTER TABLE shin_server.audit_history ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.audit_history FORCE ROW LEVEL SECURITY;
CREATE POLICY audit_history_ops ON shin_server.audit_history USING (current_setting('shin.audit_ops',true)='1');
ALTER TABLE shin_server.retrieval_journal_attempts ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.retrieval_journal_attempts FORCE ROW LEVEL SECURITY;
CREATE POLICY retrieval_journal_attempts_ops ON shin_server.retrieval_journal_attempts USING (current_setting('shin.journal_ops',true)='1');
ALTER TABLE shin_server.retrieval_journal_producers ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.retrieval_journal_producers FORCE ROW LEVEL SECURITY;
CREATE POLICY retrieval_journal_producers_ops ON shin_server.retrieval_journal_producers USING (current_setting('shin.journal_ops',true)='1');
ALTER TABLE shin_server.retrieval_journal_lifecycle ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.retrieval_journal_lifecycle FORCE ROW LEVEL SECURITY;
CREATE POLICY retrieval_journal_lifecycle_ops ON shin_server.retrieval_journal_lifecycle USING (current_setting('shin.journal_ops',true)='1');
