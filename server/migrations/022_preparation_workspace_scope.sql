-- Existing maintenance jobs keep NULL; never infer authority from old routes.
ALTER TABLE shin_server.preparations ADD COLUMN workspace_admission JSONB;
ALTER TABLE shin_server.preparations ADD CONSTRAINT preparation_workspace_admission_check
    CHECK (workspace_admission IS NULL OR
           (jsonb_typeof(workspace_admission) = 'object' AND octet_length(workspace_admission::text) <= 32768));
