#include "preparation_access.h"

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
    throw ServiceError(503, "schema-unsupported", "Preparation row security is not supported.");
}
}

void append_preparation_access_checks(SchemaChecks& checks) {
    checks.add("SELECT c.relname,p.polname,p.polcmd,COALESCE(pg_get_expr(p.polqual,p.polrelid),''), "
        "COALESCE(pg_get_expr(p.polwithcheck,p.polrelid),''), "
        "p.polpermissive AND p.polroles=ARRAY[0]::oid[] AND c.relrowsecurity AND c.relforcerowsecurity "
        "AND c.relowner=n.nspowner FROM pg_policy p JOIN pg_class c ON c.oid=p.polrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' "
        "AND c.relname IN ('preparations','preparation_retention')", [](const pqxx::result& rows) {
        using Policy = std::tuple<std::string,std::string,std::string,std::string,std::string>;
        std::set<Policy> expected, actual;
        const auto add = [&](const char* table, const char* name, const char* command, const std::string& use) {
            expected.emplace(table, name, command, compact(use), "");
        };
        const std::string ops = "((current_setting('shin.identity_ops'::text, true) = '1'::text) OR "
                                "(current_setting('shin.preparation_ops'::text, true) = '1'::text))";
        add("preparations", "preparations_ops", "*", ops);
        add("preparation_retention", "preparation_retention_ops", "*", ops);
        add("preparations", "preparations_owner", "*",
            "((principal_id = current_setting('shin.identity_principal'::text, true)) AND "
            "(project_id = current_setting('shin.content_project'::text, true)) AND "
            "(scope_tenant_id = current_setting('shin.identity_tenant'::text, true)) AND "
            "(scope_workspace_id = current_setting('shin.content_workspace'::text, true)) AND "
            "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed))");
        add("preparations", "preparations_legacy_read", "r",
            "((workspace_admission IS NULL) AND (current_setting('shin.preparation_legacy'::text, true) = '1'::text) AND "
            "(project_id = ANY (string_to_array(current_setting('shin.identity_legacy_projects'::text, true), ','::text))))");
        add("preparations", "preparations_source_read", "r",
            "((project_id = current_setting('shin.content_project'::text, true)) AND "
            "((id)::text = ANY (string_to_array(current_setting('shin.preparation_sources'::text, true), ','::text))) AND "
            "( SELECT shin_server.current_workspace_allowed() AS current_workspace_allowed))");
        add("preparation_retention", "preparation_retention_read", "r",
            "(EXISTS ( SELECT 1 FROM shin_server.preparations p WHERE (p.id = preparation_retention.preparation_id)))");
        add("preparation_retention", "preparation_retention_owner", "*",
            "(EXISTS ( SELECT 1 FROM shin_server.preparations p WHERE ((p.id = preparation_retention.preparation_id) AND "
            "(p.principal_id = current_setting('shin.identity_principal'::text, true)) AND "
            "(p.project_id = current_setting('shin.content_project'::text, true)) AND "
            "(p.scope_tenant_id = current_setting('shin.identity_tenant'::text, true)) AND "
            "(p.scope_workspace_id = current_setting('shin.content_workspace'::text, true)))))");
        for (const auto& row : rows) {
            if (!row[5].as<bool>()) unsupported();
            actual.emplace(row[0].as<std::string>(),row[1].as<std::string>(),row[2].as<std::string>(),
                           compact(row[3].as<std::string>()),compact(row[4].as<std::string>()));
        }
        if (actual != expected) unsupported();
    });
}
}
