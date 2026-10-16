ALTER TABLE shin_server.checkpoint_slots
    ADD COLUMN scope_workspace_id TEXT,
    DROP CONSTRAINT checkpoint_slots_pkey,
    ADD CONSTRAINT checkpoint_slots_workspace_fk FOREIGN KEY (project_id, scope_workspace_id)
        REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT,
    ADD CONSTRAINT checkpoint_slots_scope_check CHECK (
        scope_workspace_id IS NULL OR
        ((provenance->>'project_id' = project_id AND
          provenance->>'workspace_id' = scope_workspace_id) IS TRUE)
    );

CREATE UNIQUE INDEX checkpoint_slots_legacy_unique
    ON shin_server.checkpoint_slots(project_id, owner_id, slot_id)
    WHERE scope_workspace_id IS NULL;
CREATE UNIQUE INDEX checkpoint_slots_workspace_unique
    ON shin_server.checkpoint_slots(project_id, owner_id, scope_workspace_id, slot_id)
    WHERE scope_workspace_id IS NOT NULL;
DROP INDEX shin_server.checkpoint_slots_prune_idx;
CREATE INDEX checkpoint_slots_prune_idx
    ON shin_server.checkpoint_slots(project_id, owner_id, scope_workspace_id, state, retry_until);
