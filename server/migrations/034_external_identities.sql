-- Explicit bindings only. Do not infer external identities from legacy labels.
CREATE TABLE shin_server.identity_external_bindings (
    account_id TEXT PRIMARY KEY REFERENCES shin_server.identity_accounts(id) ON DELETE RESTRICT,
    issuer TEXT COLLATE "C" NOT NULL CHECK (octet_length(issuer) BETWEEN 9 AND 2048),
    subject TEXT COLLATE "C" NOT NULL CHECK (octet_length(subject) BETWEEN 1 AND 255),
    enabled BOOLEAN NOT NULL DEFAULT true,
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision > 0),
    UNIQUE (issuer, subject)
);
ALTER TABLE shin_server.identity_external_bindings ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_external_bindings FORCE ROW LEVEL SECURITY;
CREATE POLICY identity_external_read ON shin_server.identity_external_bindings FOR SELECT USING
    (account_id = current_setting('shin.identity_account', true));
CREATE POLICY identity_external_ops ON shin_server.identity_external_bindings FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');

ALTER TABLE shin_server.identity_audit DROP CONSTRAINT identity_audit_action_check;
ALTER TABLE shin_server.identity_audit ADD CONSTRAINT identity_audit_action_check CHECK
    (action IN ('account.created','account.state','tenant.created','membership.changed','external.bound','external.state'));
