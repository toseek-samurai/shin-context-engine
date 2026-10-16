#include "shin/server/identity_directory.h"

#include <pqxx/pqxx>
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
void append_identity_schema_checks(SchemaChecks& checks) {
    checks.add("SELECT c.relname,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull "
        "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='shin_server' AND c.relname IN ('identity_accounts','identity_tenants','identity_memberships','identity_audit') "
        "AND a.attnum>0 AND NOT a.attisdropped", [](const pqxx::result& rows) {
        using Column = std::tuple<std::string, std::string, std::string, bool>;
        const std::set<Column> expected{
            {"identity_accounts","id","text",true}, {"identity_accounts","principal_id","text",true},
            {"identity_accounts","display_name","text",true}, {"identity_accounts","enabled","boolean",true},
            {"identity_accounts","revision","bigint",true},
            {"identity_tenants","id","text",true}, {"identity_tenants","display_name","text",true},
            {"identity_tenants","owner_id","text",true},
            {"identity_memberships","tenant_id","text",true}, {"identity_memberships","account_id","text",true},
            {"identity_memberships","role","text",true}, {"identity_memberships","enabled","boolean",true},
            {"identity_memberships","revision","bigint",true},
            {"identity_audit","id","bigint",true}, {"identity_audit","actor_id","text",true},
            {"identity_audit","tenant_id","text",false}, {"identity_audit","action","text",true},
            {"identity_audit","target_id","text",true}, {"identity_audit","revision","bigint",true},
            {"identity_audit","created_at","timestamp with time zone",true}};
        std::set<Column> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(),
                                                   row[2].as<std::string>(), row[3].as<bool>());
        if (actual != expected) throw ServiceError(503, "schema-unsupported", "Identity schema is not supported.");
    });
    checks.add("SELECT "
        "(SELECT count(*)=4 AND bool_and(c.relrowsecurity AND c.relforcerowsecurity AND c.relowner=n.nspowner) FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' "
        "AND c.relname IN ('identity_accounts','identity_tenants','identity_memberships','identity_audit')) AS rls, "
        "(SELECT count(*)=8 AND bool_and(p.polpermissive AND p.polroles=ARRAY[0]::oid[]) FROM pg_policy p "
        "JOIN pg_class c ON c.oid=p.polrelid JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' "
        "AND c.relname IN ('identity_accounts','identity_tenants','identity_memberships','identity_audit')) AS policies, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_memberships'::regclass "
        "AND contype='p' AND conkey=ARRAY[1,2]::smallint[]) AS membership_pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_accounts'::regclass "
        "AND contype='u' AND conkey=ARRAY[2]::smallint[]) AS principal_unique, "
        "(SELECT count(*)=4 FROM pg_constraint WHERE conrelid IN ('shin_server.identity_tenants'::regclass, "
        "'shin_server.identity_memberships'::regclass,'shin_server.identity_audit'::regclass) AND contype='f' "
        "AND confdeltype='r') AS ownership_fks", [](const pqxx::result& rows) {
        if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "Identity schema is not supported.");
        for (const auto& field : rows[0]) if (!field.as<bool>())
            throw ServiceError(503, "schema-unsupported", "Identity schema is not supported.");
    });
    checks.add("SELECT c.relname,p.polname,p.polcmd,COALESCE(pg_get_expr(p.polqual,p.polrelid),''), "
        "COALESCE(pg_get_expr(p.polwithcheck,p.polrelid),'') FROM pg_policy p JOIN pg_class c ON c.oid=p.polrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' "
        "AND c.relname IN ('identity_accounts','identity_tenants','identity_memberships','identity_audit')",
        [](const pqxx::result& rows) {
        // Pin the deparsed policy shape as well as RLS flags; fail closed on drift.
        const auto compact = [](std::string value) {
            std::erase_if(value, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
            return value;
        };
        const std::string ops = "(current_setting('shin.identity_ops'::text, true) = '1'::text)";
        const std::string tenant = "(tenant_id = current_setting('shin.identity_tenant'::text, true))";
        const std::string manage = "(" + ops + " OR (" + tenant + " AND (current_setting('shin.identity_manage'::text, true) = '1'::text)))";
        using Policy = std::tuple<std::string,std::string,std::string,std::string,std::string>;
        std::set<Policy> expected, actual;
        const auto add = [&](const char* table, const char* name, const char* command, std::string using_expr, std::string check_expr) {
            expected.emplace(table, name, command, compact(using_expr), compact(check_expr));
        };
        add("identity_accounts", "identity_accounts_read", "r",
            "(principal_id = current_setting('shin.identity_principal'::text, true))", "");
        add("identity_accounts", "identity_accounts_ops", "*", ops, ops);
        add("identity_tenants", "identity_tenants_read", "r",
            "((owner_id = current_setting('shin.identity_account'::text, true)) AND "
            "(EXISTS ( SELECT 1 FROM shin_server.identity_memberships m WHERE ((m.tenant_id = identity_tenants.id) "
            "AND (m.account_id = current_setting('shin.identity_account'::text, true)) AND m.enabled AND (m.role = 'owner'::text)))))", "");
        add("identity_tenants", "identity_tenants_ops", "*", ops, ops);
        add("identity_memberships", "identity_memberships_read", "r",
            "(account_id = current_setting('shin.identity_account'::text, true))", "");
        add("identity_memberships", "identity_memberships_manage", "*", ops, ops);
        add("identity_audit", "identity_audit_read", "r", "(" + ops + " OR " + tenant + ")", "");
        add("identity_audit", "identity_audit_write", "a", "",
            "((actor_id = current_setting('shin.identity_principal'::text, true)) AND " + manage + ")");
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(),
            row[2].as<std::string>(), compact(row[3].as<std::string>()), compact(row[4].as<std::string>()));
        if (actual != expected) throw ServiceError(503, "schema-unsupported", "Identity policies are not supported.");
    });
}
} // namespace shin::server
