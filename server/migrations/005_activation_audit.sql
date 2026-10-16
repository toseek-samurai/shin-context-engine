ALTER TABLE shin_server.audit_events
    ADD COLUMN activation_route_id TEXT,
    ADD COLUMN activation_previous_revision BIGINT,
    ADD CONSTRAINT audit_activation_shape_check CHECK (
        (activation_route_id IS NULL AND activation_previous_revision IS NULL) OR
        (activation_route_id IS NOT NULL AND activation_previous_revision IS NOT NULL)
    ),
    ADD CONSTRAINT audit_activation_receipt_fk FOREIGN KEY
        (project_id, activation_route_id, activation_previous_revision)
        REFERENCES shin_server.activation_receipts(project_id, route_id, previous_revision) ON DELETE RESTRICT;
