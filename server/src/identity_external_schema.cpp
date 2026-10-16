#include "shin/server/identity_directory.h"
#include <pqxx/pqxx>
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
void append_external_identity_schema_checks(SchemaChecks& checks) {
    checks.add("SELECT a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,"
        "COALESCE(c.collname,'') FROM pg_attribute a LEFT JOIN pg_collation c ON c.oid=a.attcollation "
        "WHERE a.attrelid='shin_server.identity_external_bindings'::regclass AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
        using Column = std::tuple<std::string,std::string,bool,std::string>;
        const std::set<Column> expected{{"account_id","text",true,"default"}, {"issuer","text",true,"C"},
            {"subject","text",true,"C"}, {"enabled","boolean",true,""}, {"revision","bigint",true,""}};
        std::set<Column> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(),
                                                   row[2].as<bool>(), row[3].as<std::string>());
        if (actual != expected) throw ServiceError(503, "schema-unsupported", "External identity schema is not supported.");
    });
    checks.add("SELECT "
        "(SELECT count(*)=1 AND bool_and(c.relrowsecurity AND c.relforcerowsecurity AND c.relowner=n.nspowner) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE c.oid='shin_server.identity_external_bindings'::regclass) AS rls, "
        "(SELECT count(*)=2 AND bool_and(polpermissive AND polroles=ARRAY[0]::oid[]) FROM pg_policy "
        "WHERE polrelid='shin_server.identity_external_bindings'::regclass) AS policies, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_external_bindings'::regclass "
        "AND contype='p' AND conkey=ARRAY[1]::smallint[] AND convalidated AND NOT condeferrable) AS pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_external_bindings'::regclass "
        "AND contype='u' AND conkey=ARRAY[2,3]::smallint[] AND convalidated AND NOT condeferrable) AS unique_key, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_external_bindings'::regclass "
        "AND contype='f' AND conkey=ARRAY[1]::smallint[] AND confkey=ARRAY[1]::smallint[] "
        "AND confrelid='shin_server.identity_accounts'::regclass AND confdeltype='r' "
        "AND convalidated AND NOT condeferrable) AS account_fk", [](const pqxx::result& rows) {
        if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "External identity schema is not supported.");
        for (const auto& value : rows[0]) if (!value.as<bool>())
            throw ServiceError(503, "schema-unsupported", "External identity keys or RLS are not supported.");
    });
    // Migrations/recovery also validate layout as the schema owner. Runtime
    // connection admission independently rejects owners and BYPASSRLS roles.
    checks.add("SELECT current_user=pg_get_userbyid(nspowner) OR ("
        "NOT has_table_privilege(current_user,'shin_server.identity_external_bindings','DELETE,TRUNCATE,UPDATE') AND "
        "has_table_privilege(current_user,'shin_server.identity_external_bindings','SELECT') AND "
        "has_table_privilege(current_user,'shin_server.identity_external_bindings','INSERT') AND "
        "has_column_privilege(current_user,'shin_server.identity_external_bindings','enabled','UPDATE') AND "
        "has_column_privilege(current_user,'shin_server.identity_external_bindings','revision','UPDATE') AND "
        "NOT has_column_privilege(current_user,'shin_server.identity_external_bindings','account_id','UPDATE') AND "
        "NOT has_column_privilege(current_user,'shin_server.identity_external_bindings','issuer','UPDATE') AND "
        "NOT has_column_privilege(current_user,'shin_server.identity_external_bindings','subject','UPDATE')) "
        "FROM pg_namespace WHERE nspname='shin_server'",
        [](const pqxx::result& rows) {
        if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "External identity schema is not supported.");
        for (const auto& value : rows[0]) if (!value.as<bool>())
            throw ServiceError(503, "schema-unsupported", "External identity keys, RLS or grants are not supported.");
    });
    checks.add("SELECT polname,polcmd,COALESCE(pg_get_expr(polqual,polrelid),''),"
        "COALESCE(pg_get_expr(polwithcheck,polrelid),'') FROM pg_policy "
        "WHERE polrelid='shin_server.identity_external_bindings'::regclass", [](const pqxx::result& rows) {
        const auto compact = [](std::string text) {
            std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
            return text;
        };
        const auto ops = compact("(current_setting('shin.identity_ops'::text, true) = '1'::text)");
        using Policy = std::tuple<std::string,std::string,std::string,std::string>;
        const std::set<Policy> expected{
            {"identity_external_read","r",compact("(account_id = current_setting('shin.identity_account'::text, true))"),""},
            {"identity_external_ops","*",ops,ops}};
        std::set<Policy> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(),
            compact(row[2].as<std::string>()), compact(row[3].as<std::string>()));
        if (actual != expected) throw ServiceError(503, "schema-unsupported", "External identity policies are not supported.");
    });
}
} // namespace shin::server
