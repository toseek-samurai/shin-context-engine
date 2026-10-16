-- Additive directory only: no existing project/workspace is assigned an owner.
CREATE TABLE shin_server.identity_accounts (
    id TEXT PRIMARY KEY CHECK (id ~ '^[a-z][a-z0-9-]{0,62}$'),
    principal_id TEXT NOT NULL UNIQUE CHECK (principal_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    display_name TEXT NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 120),
    enabled BOOLEAN NOT NULL DEFAULT true,
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision > 0)
);
CREATE TABLE shin_server.identity_tenants (
    id TEXT PRIMARY KEY CHECK (id ~ '^[a-z][a-z0-9-]{0,62}$'),
    display_name TEXT NOT NULL CHECK (octet_length(display_name) BETWEEN 1 AND 120),
    owner_id TEXT NOT NULL REFERENCES shin_server.identity_accounts(id) ON DELETE RESTRICT
);
CREATE TABLE shin_server.identity_memberships (
    tenant_id TEXT NOT NULL REFERENCES shin_server.identity_tenants(id) ON DELETE RESTRICT,
    account_id TEXT NOT NULL REFERENCES shin_server.identity_accounts(id) ON DELETE RESTRICT,
    role TEXT NOT NULL CHECK (role IN ('owner','maintainer','reader')),
    enabled BOOLEAN NOT NULL,
    revision BIGINT NOT NULL CHECK (revision > 0),
    PRIMARY KEY (tenant_id, account_id)
);
CREATE INDEX identity_memberships_account_idx ON shin_server.identity_memberships(account_id, tenant_id);
CREATE TABLE shin_server.identity_audit (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    actor_id TEXT NOT NULL CHECK (actor_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    tenant_id TEXT REFERENCES shin_server.identity_tenants(id) ON DELETE RESTRICT,
    action TEXT NOT NULL CHECK (action IN ('account.created','account.state','tenant.created','membership.changed')),
    target_id TEXT NOT NULL CHECK (target_id ~ '^[a-z][a-z0-9-]{0,62}$'),
    revision BIGINT NOT NULL CHECK (revision > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);
CREATE INDEX identity_audit_tenant_idx ON shin_server.identity_audit(tenant_id, id);

ALTER TABLE shin_server.identity_accounts ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_accounts FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_tenants ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_tenants FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_memberships ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_memberships FORCE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_audit ENABLE ROW LEVEL SECURITY;
ALTER TABLE shin_server.identity_audit FORCE ROW LEVEL SECURITY;

CREATE POLICY identity_accounts_read ON shin_server.identity_accounts FOR SELECT USING
    (principal_id = current_setting('shin.identity_principal', true));
CREATE POLICY identity_accounts_ops ON shin_server.identity_accounts FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');
CREATE POLICY identity_tenants_read ON shin_server.identity_tenants FOR SELECT USING
    (EXISTS (SELECT 1 FROM shin_server.identity_memberships m WHERE m.tenant_id = identity_tenants.id
        AND m.account_id = current_setting('shin.identity_account', true) AND m.enabled));
CREATE POLICY identity_tenants_ops ON shin_server.identity_tenants FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1') WITH CHECK
    (current_setting('shin.identity_ops', true) = '1');
CREATE POLICY identity_memberships_read ON shin_server.identity_memberships FOR SELECT USING
    (account_id = current_setting('shin.identity_account', true)
        OR tenant_id = current_setting('shin.identity_tenant', true));
CREATE POLICY identity_memberships_manage ON shin_server.identity_memberships FOR ALL USING
    (current_setting('shin.identity_ops', true) = '1' OR
        (tenant_id = current_setting('shin.identity_tenant', true) AND current_setting('shin.identity_manage', true) = '1'))
    WITH CHECK (current_setting('shin.identity_ops', true) = '1' OR
        (tenant_id = current_setting('shin.identity_tenant', true) AND current_setting('shin.identity_manage', true) = '1'));
CREATE POLICY identity_audit_read ON shin_server.identity_audit FOR SELECT USING
    (current_setting('shin.identity_ops', true) = '1' OR tenant_id = current_setting('shin.identity_tenant', true));
CREATE POLICY identity_audit_write ON shin_server.identity_audit FOR INSERT WITH CHECK
    (actor_id = current_setting('shin.identity_principal', true) AND
        (current_setting('shin.identity_ops', true) = '1' OR
        (tenant_id = current_setting('shin.identity_tenant', true) AND current_setting('shin.identity_manage', true) = '1')));
