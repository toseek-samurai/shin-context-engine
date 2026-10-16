ALTER TABLE shin_server.checkpoint_slots
    ADD COLUMN receipt_origins JSONB NOT NULL DEFAULT '[]'::jsonb;

ALTER TABLE shin_server.checkpoint_slots
    ADD CONSTRAINT checkpoint_slots_origins_check CHECK (
        jsonb_typeof(receipt_origins) = 'array' AND
        jsonb_array_length(receipt_origins) <= 32 AND
        octet_length(receipt_origins::text) <= 2304 AND
        (state = 'active' OR receipt_origins = '[]'::jsonb)
    );

ALTER TABLE shin_server.checkpoint_slots DROP CONSTRAINT checkpoint_slots_receipt_check;
ALTER TABLE shin_server.checkpoint_slots
    ADD CONSTRAINT checkpoint_slots_receipt_check CHECK (
        (last_operation = 'save' OR last_operation = 'clear' OR last_operation = 'receipt-import') AND
        last_request_id ~ '^[0-9a-f]{32}$' AND last_request_hash ~ '^[0-9a-f]{64}$' AND
        last_expected_revision >= 0 AND last_request_deadline BETWEEN 0 AND 253402300799 AND
        retry_until BETWEEN last_request_deadline AND 253402300799
    );
