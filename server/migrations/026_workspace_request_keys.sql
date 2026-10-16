ALTER TABLE shin_server.workspace_requests
    DROP CONSTRAINT workspace_requests_pkey,
    ADD CONSTRAINT workspace_requests_pkey PRIMARY KEY (principal_id, project_id, key);
