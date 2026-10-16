#include "shin/server/identity_directory.h"
#include <pqxx/pqxx>
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
namespace {
[[noreturn]] void drift() { throw ServiceError(503, "schema-unsupported", "Device session schema is not supported."); }
std::string compact(std::string text) {
    std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
    return text;
}
}
void append_device_schema_checks(SchemaChecks& checks) {
    checks.add("SELECT a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,COALESCE(c.collname,'') "
        "FROM pg_attribute a LEFT JOIN pg_collation c ON c.oid=a.attcollation "
        "WHERE a.attrelid='shin_server.identity_devices'::regclass AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
        using Column = std::tuple<std::string,std::string,bool,std::string>;
        const std::set<Column> expected{{"id","text",true,"default"}, {"account_id","text",true,"default"},
            {"client_id","text",true,"C"}, {"session_id","text",true,"C"}, {"display_name","text",true,"default"},
            {"revoked","boolean",true,""}, {"revision","bigint",true,""}, {"created_at","timestamp with time zone",true,""}};
        std::set<Column> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(),row[1].as<std::string>(),row[2].as<bool>(),row[3].as<std::string>());
        if (actual != expected) drift();
    });
    checks.add("SELECT "
        "(SELECT count(*)=1 AND bool_and(c.relrowsecurity AND c.relforcerowsecurity AND c.relowner=n.nspowner) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE c.oid='shin_server.identity_devices'::regclass) AS rls, "
        "(SELECT count(*)=3 AND bool_and(polpermissive AND polroles=ARRAY[0]::oid[]) FROM pg_policy "
        "WHERE polrelid='shin_server.identity_devices'::regclass) AS policies, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_devices'::regclass "
        "AND contype='p' AND conkey=ARRAY[1]::smallint[] AND convalidated AND NOT condeferrable) AS pk, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_devices'::regclass "
        "AND contype='u' AND conkey=ARRAY[2,3,4]::smallint[] AND convalidated AND NOT condeferrable) AS session_key, "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.identity_devices'::regclass "
        "AND contype='f' AND conkey=ARRAY[2]::smallint[] AND confkey=ARRAY[1]::smallint[] "
        "AND confrelid='shin_server.identity_external_bindings'::regclass AND confdeltype='r' "
        "AND convalidated AND NOT condeferrable) AS account_fk, "
        "EXISTS(SELECT 1 FROM pg_index WHERE indexrelid='shin_server.identity_devices_account_id'::regclass "
        "AND indrelid='shin_server.identity_devices'::regclass AND indkey='2 1'::int2vector "
        "AND indisvalid AND indisready AND indpred IS NULL AND indexprs IS NULL) AS paging_index",
        [](const pqxx::result& rows) {
        if (rows.size() != 1) drift();
        for (const auto& value : rows[0]) if (value.is_null() || !value.as<bool>()) drift();
    });
    checks.add("SELECT polname,polcmd,COALESCE(pg_get_expr(polqual,polrelid),''),COALESCE(pg_get_expr(polwithcheck,polrelid),'') "
        "FROM pg_policy WHERE polrelid='shin_server.identity_devices'::regclass", [](const pqxx::result& rows) {
        const auto own = compact("(account_id = current_setting('shin.identity_account'::text, true))");
        const auto ops = compact("(current_setting('shin.identity_ops'::text, true) = '1'::text)");
        using Policy = std::tuple<std::string,std::string,std::string,std::string>;
        const std::set<Policy> expected{{"identity_devices_read","r",own,""}, {"identity_devices_ops","*",ops,ops},
            {"identity_devices_revoke","w",own,compact("((account_id = current_setting('shin.identity_account'::text, true)) AND revoked)")}};
        std::set<Policy> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(),row[1].as<std::string>(),compact(row[2].as<std::string>()),compact(row[3].as<std::string>()));
        if (actual != expected) drift();
    });
    checks.add("SELECT current_user=pg_get_userbyid(nspowner) OR ("
        "has_table_privilege(current_user,'shin_server.identity_devices','SELECT') AND "
        "has_table_privilege(current_user,'shin_server.identity_devices','INSERT') AND "
        "NOT has_table_privilege(current_user,'shin_server.identity_devices','DELETE,TRUNCATE,UPDATE') AND "
        "has_column_privilege(current_user,'shin_server.identity_devices','revoked','UPDATE') AND "
        "has_column_privilege(current_user,'shin_server.identity_devices','revision','UPDATE') AND "
        "NOT EXISTS(SELECT 1 FROM pg_attribute a WHERE a.attrelid='shin_server.identity_devices'::regclass "
        "AND a.attnum>0 AND NOT a.attisdropped AND a.attname NOT IN ('revoked','revision') "
        "AND has_column_privilege(current_user,a.attrelid,a.attnum,'UPDATE'))) FROM pg_namespace WHERE nspname='shin_server'",
        [](const pqxx::result& rows) { if (rows.size() != 1 || !rows[0][0].as<bool>()) drift(); });
}
} // namespace shin::server
