CREATE TABLE shin_server.server_recovery (
    singleton BOOLEAN PRIMARY KEY CHECK (singleton),
    last_recovery JSONB CHECK (
        last_recovery IS NULL OR (
            jsonb_typeof(last_recovery) = 'object' AND
            octet_length(last_recovery::text) <= 16384
        )
    )
);
INSERT INTO shin_server.server_recovery(singleton) VALUES (TRUE);
