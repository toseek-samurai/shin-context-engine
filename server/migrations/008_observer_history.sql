ALTER TABLE shin_server.observer_streams
    DROP CONSTRAINT observer_streams_accepted_sequence_check,
    ADD COLUMN confirmed_sequence BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN retired_sequence BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN last_retention JSONB,
    ADD CONSTRAINT observer_streams_accepted_sequence_check
        CHECK (accepted_sequence BETWEEN 0 AND 9223372036854775806),
    ADD CONSTRAINT observer_streams_retention_cursors_check CHECK (
        retired_sequence >= 0 AND retired_sequence <= confirmed_sequence AND
        confirmed_sequence <= accepted_sequence AND
        (retired_sequence = 0 OR retired_sequence < confirmed_sequence) AND
        accepted_sequence - retired_sequence <= 1024
    ),
    ADD CONSTRAINT observer_streams_last_retention_check CHECK (
        last_retention IS NULL OR (
            jsonb_typeof(last_retention) = 'object' AND
            octet_length(last_retention::text) <= 8192
        )
    );

ALTER TABLE shin_server.observer_submissions
    DROP CONSTRAINT observer_submissions_sequence_check,
    ADD CONSTRAINT observer_submissions_sequence_check
        CHECK (sequence BETWEEN 1 AND 9223372036854775806);

ALTER TABLE shin_server.audit_events
    DROP CONSTRAINT audit_observer_submission_fk;
