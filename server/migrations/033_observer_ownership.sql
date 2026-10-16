ALTER TABLE shin_server.observer_streams ADD COLUMN scope_tenant_id text,
 ADD CONSTRAINT observer_streams_owner_fk FOREIGN KEY(scope_tenant_id,project_id)
 REFERENCES shin_server.identity_project_owners(tenant_id,project_id) ON DELETE RESTRICT;
ALTER TABLE shin_server.observer_tombstones ADD COLUMN scope_tenant_id text,
 ADD CONSTRAINT observer_tombstones_owner_fk FOREIGN KEY(scope_tenant_id,project_id)
 REFERENCES shin_server.identity_project_owners(tenant_id,project_id) ON DELETE RESTRICT;

ALTER TABLE shin_server.observer_streams ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_streams FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_submissions ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_submissions FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_tombstones ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_tombstones FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_usage ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.observer_usage FORCE ROW LEVEL SECURITY;
CREATE POLICY observer_streams_ops ON shin_server.observer_streams USING (current_setting('shin.observer_ops',true)='1');
CREATE POLICY observer_tombstones_ops ON shin_server.observer_tombstones USING (current_setting('shin.observer_ops',true)='1');
CREATE POLICY observer_submissions_ops ON shin_server.observer_submissions USING (current_setting('shin.observer_ops',true)='1');
CREATE POLICY observer_usage_quota ON shin_server.observer_usage USING (current_setting('shin.observer_quota',true)='1');
CREATE POLICY observer_streams_legacy ON shin_server.observer_streams USING (
 scope_tenant_id IS NULL AND current_setting('shin.observer_legacy',true)='1'
 AND principal_id=current_setting('shin.identity_principal',true)
 AND project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects',true),','))
);
CREATE POLICY observer_tombstones_legacy ON shin_server.observer_tombstones FOR SELECT USING (
 scope_tenant_id IS NULL AND current_setting('shin.observer_legacy',true)='1'
 AND principal_id=current_setting('shin.identity_principal',true)
 AND project_id=ANY(string_to_array(current_setting('shin.identity_legacy_projects',true),','))
);
CREATE POLICY observer_streams_owner ON shin_server.observer_streams USING (
 principal_id=current_setting('shin.identity_principal',true)
 AND project_id=current_setting('shin.content_project',true)
 AND workspace_id=current_setting('shin.content_workspace',true)
 AND scope_tenant_id=current_setting('shin.identity_tenant',true)
 AND (SELECT shin_server.current_workspace_allowed())
);
CREATE POLICY observer_tombstones_owner ON shin_server.observer_tombstones USING (
 principal_id=current_setting('shin.identity_principal',true)
 AND project_id=current_setting('shin.content_project',true)
 AND workspace_id=current_setting('shin.content_workspace',true)
 AND scope_tenant_id=current_setting('shin.identity_tenant',true)
 AND (SELECT shin_server.current_workspace_allowed())
);
CREATE POLICY observer_submissions_stream ON shin_server.observer_submissions USING (
 EXISTS (SELECT 1 FROM shin_server.observer_streams s WHERE s.stream_id=observer_submissions.stream_id)
);
