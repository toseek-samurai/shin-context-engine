-- A device entry denotes one authenticated issuer session, not hardware proof.
CREATE TABLE shin_server.identity_devices (
    id TEXT PRIMARY KEY DEFAULT ('d' || replace(gen_random_uuid()::text, '-', '')),
    account_id TEXT NOT NULL REFERENCES shin_server.identity_external_bindings(account_id) ON DELETE RESTRICT,
    client_id TEXT COLLATE "C" NOT NULL CHECK (octet_length(client_id) BETWEEN 1 AND 255),
    session_id TEXT COLLATE "C" NOT NULL CHECK (octet_length(session_id) BETWEEN 1 AND 255),
    display_name TEXT NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 120),
    revoked BOOLEAN NOT NULL DEFAULT false,
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (account_id, client_id, session_id)
);
CREATE INDEX identity_devices_account_id ON shin_server.identity_devices(account_id,id);
ALTER TABLE shin_server.identity_devices ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_devices FORCE ROW LEVEL SECURITY;
CREATE POLICY identity_devices_read ON shin_server.identity_devices FOR SELECT USING
    (account_id = current_setting('shin.identity_account', true));
CREATE POLICY identity_devices_revoke ON shin_server.identity_devices FOR UPDATE USING
    (account_id = current_setting('shin.identity_account', true)) WITH CHECK
    (account_id = current_setting('shin.identity_account', true) AND revoked);
CREATE POLICY identity_devices_ops ON shin_server.identity_devices FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');

ALTER TABLE shin_server.identity_audit DROP CONSTRAINT identity_audit_action_check;
ALTER TABLE shin_server.identity_audit ADD CONSTRAINT identity_audit_action_check CHECK
    (action IN ('account.created','account.state','tenant.created','membership.changed',
                'external.bound','external.state','device.registered','device.revoked'));
