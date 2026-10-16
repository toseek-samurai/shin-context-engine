ALTER TABLE shin_server.retrieval_journal_producers
    ADD COLUMN retired_through BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN retired_count BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN retired_missing_count BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN last_retention JSONB,
    ADD CONSTRAINT retrieval_journal_producers_retirement_check CHECK (
        retired_through >= 0 AND retired_count >= 0 AND retired_missing_count >= 0 AND
        retired_count + retired_missing_count = retired_through AND
        retired_through <= observed_through AND
        retired_count <= captured_count AND
        retired_missing_count <= missing_count
    ),
    ADD CONSTRAINT retrieval_journal_producers_last_retention_check CHECK (
        last_retention IS NULL OR (
            jsonb_typeof(last_retention) = 'object' AND
            octet_length(last_retention::text) <= 65536
        )
    );
