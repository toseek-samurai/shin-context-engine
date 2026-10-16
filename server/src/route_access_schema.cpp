#include "route_access.h"
#include "shin/server/route_history_security_migration.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
namespace {
std::string compact(std::string value) {
    std::erase_if(value, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
    return value;
}
[[noreturn]] void unsupported() {
    throw ServiceError(503, "schema-unsupported", "Route/history security is not supported.");
}
}
void append_route_history_checks(SchemaChecks& checks) {
    checks.add("SELECT c.relname,p.polname,p.polcmd,COALESCE(pg_get_expr(p.polqual,p.polrelid),''),"
        "COALESCE(pg_get_expr(p.polwithcheck,p.polrelid),''),"
        "p.polpermissive AND p.polroles=ARRAY[0]::oid[] AND c.relrowsecurity AND c.relforcerowsecurity "
        "AND c.relowner=n.nspowner FROM pg_policy p JOIN pg_class c ON c.oid=p.polrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' AND c.relname IN "
        "('active_routes','activation_receipts','audit_events','audit_history','retrieval_journal_attempts',"
        "'retrieval_journal_producers','retrieval_journal_lifecycle',"
        "'observer_streams','observer_tombstones','observer_submissions','observer_usage')", [](const pqxx::result& rows) {
        using Policy = std::tuple<std::string,std::string,std::string,std::string,std::string>;
        std::set<Policy> expected, actual;
        const auto add = [&](const std::string& table, const std::string& suffix, const char* command,
                             const std::string& use, const std::string& check = "") {
            expected.emplace(table, table + suffix, command, compact(use), compact(check));
        };
        const auto setting = [](const std::string& key) {
            return "(current_setting('shin." + key + "'::text, true) = '1'::text)";
        };
        const std::string use = "((project_id = current_setting('shin.content_project'::text, true)) AND "
            "(((scope_workspace_id IS NULL) AND (route_id = current_setting('shin.content_route'::text, true))) OR "
            "(scope_workspace_id = current_setting('shin.content_workspace'::text, true))) AND "
            "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed))";
        for (const std::string table : {"active_routes", "activation_receipts"}) {
            add(table, "_ops", "*", setting("identity_ops"));
            add(table, "_references", "r", table == "active_routes" ?
                "(" + setting("route_refs") + " OR " + setting("preparation_ops") + ")" : setting("route_refs"));
            add(table, "_legacy", "r", "((scope_workspace_id IS NULL) AND " + setting("preparation_legacy") +
                " AND (project_id = ANY (string_to_array(current_setting('shin.identity_legacy_projects'::text, true), ','::text))))");
            add(table, "_owner", "*", use,
                std::string("((project_id = current_setting('shin.content_project'::text, true)) AND "
                "(route_id = current_setting('shin.content_route'::text, true)) AND ") +
                (table == "activation_receipts" ? "(principal_id = current_setting('shin.identity_principal'::text, true)) AND " : "") +
                "(scope_workspace_id = current_setting('shin.content_workspace'::text, true)) AND "
                "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed) AND "
                "((preparation_id IS NULL) OR (EXISTS ( SELECT 1 FROM shin_server.preparations p WHERE "
                "((p.id = " + table + ".preparation_id) AND (p.project_id = " + table + ".project_id) AND "
                "(p.scope_workspace_id = " + table + ".scope_workspace_id) AND "
                "(p.principal_id = current_setting('shin.identity_principal'::text, true)))))))");
        }
        for (const std::string table : {"audit_events", "audit_history"}) add(table, "_ops", "*", setting("audit_ops"));
        add("audit_events", "_append", "a", "", setting("audit_append"));
        add("audit_events", "_legacy", "r",
            "(project_id = ANY (string_to_array(current_setting('shin.audit_projects'::text, true), ','::text)))");
        for (const std::string table : {"retrieval_journal_attempts", "retrieval_journal_producers", "retrieval_journal_lifecycle"})
            add(table, "_ops", "*", setting("journal_ops"));
        add("retrieval_journal_attempts", "_owner", "r",
            "((scope_principal_id = current_setting('shin.identity_principal'::text, true)) AND "
            "(project_id = current_setting('shin.content_project'::text, true)) AND "
            "(scope_tenant_id = current_setting('shin.identity_tenant'::text, true)) AND "
            "(scope_workspace_id = current_setting('shin.content_workspace'::text, true)) AND "
            "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed))");
        for (const std::string table : {"observer_streams", "observer_tombstones"}) {
            add(table, "_ops", "*", setting("observer_ops"));
            add(table, "_legacy", table == "observer_streams" ? "*" : "r",
                "((scope_tenant_id IS NULL) AND " + setting("observer_legacy") +
                " AND (principal_id = current_setting('shin.identity_principal'::text, true)) AND "
                "(project_id = ANY (string_to_array(current_setting('shin.identity_legacy_projects'::text, true), ','::text))))");
            add(table, "_owner", "*",
                "((principal_id = current_setting('shin.identity_principal'::text, true)) AND "
                "(project_id = current_setting('shin.content_project'::text, true)) AND "
                "(workspace_id = current_setting('shin.content_workspace'::text, true)) AND "
                "(scope_tenant_id = current_setting('shin.identity_tenant'::text, true)) AND "
                "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed))");
        }
        add("observer_submissions", "_ops", "*", setting("observer_ops"));
        add("observer_submissions", "_stream", "*", "(EXISTS ( SELECT 1 FROM shin_server.observer_streams s "
            "WHERE (s.stream_id = observer_submissions.stream_id)))");
        add("observer_usage", "_quota", "*", setting("observer_quota"));
        for (const auto& row : rows) {
            if (!row[5].as<bool>()) unsupported();
            actual.emplace(row[0].as<std::string>(),row[1].as<std::string>(),row[2].as<std::string>(),
                           compact(row[3].as<std::string>()),compact(row[4].as<std::string>()));
        }
        if (actual != expected) unsupported();
    });
    checks.add("SELECT p.prosrc,p.prosecdef=false AND p.provolatile='s' AND NOT p.proleakproof AND p.prokind='f' "
        "AND p.pronargs=0 AND p.prorettype='boolean'::regtype AND l.lanname='sql' AND p.proowner=n.nspowner "
        "AND p.proconfig=ARRAY['search_path=pg_catalog']::text[] "
        "AND has_function_privilege(current_user,p.oid,'EXECUTE') "
        "AND NOT EXISTS(SELECT 1 FROM aclexplode(COALESCE(p.proacl,acldefault('f',p.proowner))) a "
        "WHERE a.grantee=0 OR (a.grantee<>p.proowner AND a.is_grantable)), "
        "(SELECT count(*)=2 FROM pg_constraint WHERE conname IN ('active_routes_workspace_fk','activation_receipts_workspace_fk') "
        "AND conrelid IN ('shin_server.active_routes'::regclass,'shin_server.activation_receipts'::regclass) "
        "AND confrelid='shin_server.identity_workspace_owners'::regclass AND contype='f' AND confdeltype='r' AND convalidated "
        "AND confkey=ARRAY[2,3]::smallint[] AND conkey=ARRAY[1,(SELECT attnum FROM pg_attribute "
        "WHERE attrelid=conrelid AND attname='scope_workspace_id' AND NOT attisdropped)]::smallint[]) "
        "FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace JOIN pg_language l ON l.oid=p.prolang "
        "WHERE n.nspname='shin_server' AND p.proname='current_workspace_allowed'", [](const pqxx::result& rows) {
        if (rows.size() != 1 || !rows[0][1].as<bool>() || !rows[0][2].as<bool>()) unsupported();
        const std::string_view migration{generated_migrations::route_history_security_sql};
        const auto start = migration.find("$body$") + 6;
        const auto end = migration.find("$body$", start);
        if (rows[0][0].as<std::string>() != migration.substr(start, end - start)) unsupported();
    });
}
}
