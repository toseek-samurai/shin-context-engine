CREATE TABLE shin_server.audit_history (
    singleton BOOLEAN NOT NULL,
    retired_through BIGINT NOT NULL DEFAULT 0,
    retired_events BIGINT NOT NULL DEFAULT 0,
    last_retention JSONB,
    CONSTRAINT audit_history_pkey PRIMARY KEY (singleton),
    CONSTRAINT audit_history_singleton_check CHECK (singleton),
    CONSTRAINT audit_history_retired_through_check
        CHECK (retired_through BETWEEN 0 AND 9223372036854775806),
    CONSTRAINT audit_history_retired_events_check
        CHECK (retired_events BETWEEN 0 AND retired_through),
    CONSTRAINT audit_history_last_retention_check CHECK (
        last_retention IS NULL OR (
            jsonb_typeof(last_retention) = 'object' AND
            octet_length(last_retention::text) <= 16384
        )
    ),
    CONSTRAINT audit_history_shape_check CHECK (
        (retired_through = 0 AND retired_events = 0 AND last_retention IS NULL) OR
        (retired_through > 0 AND retired_events > 0 AND last_retention IS NOT NULL)
    )
);

INSERT INTO shin_server.audit_history (singleton) VALUES (TRUE);
