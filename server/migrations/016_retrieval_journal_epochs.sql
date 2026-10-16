ALTER TABLE shin_server.retrieval_journal_producers ADD COLUMN producer_epoch BIGINT;
WITH numbered AS (
    SELECT instance_id, row_number() OVER (ORDER BY created_at, instance_id) AS epoch
    FROM shin_server.retrieval_journal_producers
)
UPDATE shin_server.retrieval_journal_producers p SET producer_epoch=n.epoch
FROM numbered n WHERE n.instance_id=p.instance_id;
ALTER TABLE shin_server.retrieval_journal_producers
    ALTER COLUMN producer_epoch SET NOT NULL,
    ADD CONSTRAINT retrieval_journal_producers_epoch_check CHECK (producer_epoch > 0),
    ADD CONSTRAINT retrieval_journal_producers_epoch_unique UNIQUE (producer_epoch),
    ADD CONSTRAINT retrieval_journal_producers_instance_epoch_unique UNIQUE (instance_id, producer_epoch);

ALTER TABLE shin_server.retrieval_journal_attempts ADD COLUMN producer_epoch BIGINT;
UPDATE shin_server.retrieval_journal_attempts j SET producer_epoch=p.producer_epoch
FROM shin_server.retrieval_journal_producers p WHERE p.instance_id=j.instance_id;
ALTER TABLE shin_server.retrieval_journal_attempts
    ALTER COLUMN producer_epoch SET NOT NULL,
    DROP CONSTRAINT retrieval_journal_attempts_instance_fk,
    ADD CONSTRAINT retrieval_journal_attempts_epoch_check CHECK (producer_epoch > 0),
    ADD CONSTRAINT retrieval_journal_attempts_instance_fk FOREIGN KEY (instance_id, producer_epoch)
        REFERENCES shin_server.retrieval_journal_producers(instance_id, producer_epoch) ON DELETE RESTRICT,
    DROP CONSTRAINT retrieval_journal_attempts_retrieval_mode_check,
    ADD CONSTRAINT retrieval_journal_attempts_retrieval_mode_check CHECK (
        retrieval_mode IS NULL OR retrieval_mode = 'strict' OR retrieval_mode = 'relaxed' OR
        retrieval_mode = 'owner-scoped' OR retrieval_mode = 'directory-scoped' OR
        retrieval_mode = 'planned' OR retrieval_mode = 'identifier-scoped'
    );

CREATE TABLE shin_server.retrieval_journal_lifecycle (
    singleton BOOLEAN NOT NULL DEFAULT true,
    allocated_through BIGINT NOT NULL DEFAULT 0,
    reclaimed_through BIGINT NOT NULL DEFAULT 0,
    reclaimed_producer_count BIGINT NOT NULL DEFAULT 0,
    reclaimed_captured_count BIGINT NOT NULL DEFAULT 0,
    reclaimed_missing_count BIGINT NOT NULL DEFAULT 0,
    last_reclamation JSONB,
    CONSTRAINT retrieval_journal_lifecycle_pkey PRIMARY KEY (singleton),
    CONSTRAINT retrieval_journal_lifecycle_singleton_check CHECK (singleton),
    CONSTRAINT retrieval_journal_lifecycle_progress_check CHECK (
        allocated_through >= 0 AND reclaimed_through >= 0 AND reclaimed_through <= allocated_through AND
        reclaimed_producer_count >= 0 AND reclaimed_producer_count <= reclaimed_through AND
        reclaimed_captured_count >= 0 AND reclaimed_missing_count >= 0
    ),
    CONSTRAINT retrieval_journal_lifecycle_receipt_check CHECK (
        last_reclamation IS NULL OR (
            jsonb_typeof(last_reclamation) = 'object' AND octet_length(last_reclamation::text) <= 65536
        )
    )
);
INSERT INTO shin_server.retrieval_journal_lifecycle(singleton, allocated_through)
SELECT true, COALESCE(max(producer_epoch), 0) FROM shin_server.retrieval_journal_producers;
