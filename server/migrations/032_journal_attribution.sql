ALTER TABLE shin_server.retrieval_journal_attempts
 ADD COLUMN scope_principal_id text,
 ADD COLUMN scope_tenant_id text,
 ADD COLUMN scope_workspace_id text,
 ADD CONSTRAINT retrieval_journal_attempts_scope_check CHECK (
   (scope_principal_id IS NULL AND scope_tenant_id IS NULL AND scope_workspace_id IS NULL)
   OR (scope_principal_id IS NOT NULL AND scope_tenant_id IS NOT NULL AND scope_workspace_id IS NOT NULL)
 ),
 ADD CONSTRAINT retrieval_journal_attempts_selection_scope_check CHECK (
   scope_workspace_id IS NULL OR workspace_id IS NULL OR workspace_id=scope_workspace_id
 ),
 ADD CONSTRAINT retrieval_journal_attempts_principal_fk FOREIGN KEY(scope_principal_id)
 REFERENCES shin_server.identity_accounts(principal_id) ON DELETE RESTRICT,
 ADD CONSTRAINT retrieval_journal_attempts_tenant_fk FOREIGN KEY(scope_tenant_id,project_id)
 REFERENCES shin_server.identity_project_owners(tenant_id,project_id) ON DELETE RESTRICT,
 ADD CONSTRAINT retrieval_journal_attempts_workspace_fk FOREIGN KEY(project_id,scope_workspace_id)
 REFERENCES shin_server.identity_workspace_owners(project_id,workspace_id) ON DELETE RESTRICT;

CREATE INDEX retrieval_journal_attempts_scope_cursor_idx ON shin_server.retrieval_journal_attempts
 (scope_principal_id,project_id,scope_tenant_id,scope_workspace_id,id) WHERE scope_principal_id IS NOT NULL;
CREATE POLICY retrieval_journal_attempts_owner ON shin_server.retrieval_journal_attempts FOR SELECT USING (
 scope_principal_id=current_setting('shin.identity_principal',true)
 AND project_id=current_setting('shin.content_project',true)
 AND scope_tenant_id=current_setting('shin.identity_tenant',true)
 AND scope_workspace_id=current_setting('shin.content_workspace',true)
 AND (SELECT shin_server.current_workspace_allowed())
);
