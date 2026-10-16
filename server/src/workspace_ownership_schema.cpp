#include "shin/server/identity_directory.h"
#include <pqxx/pqxx>
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
namespace {
constexpr auto tables = "('identity_project_owners','identity_workspace_owners','identity_ownership_audit')";
std::string compact(std::string value) {
    std::erase_if(value, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
    return value;
}
[[noreturn]] void unsupported() { throw ServiceError(503, "schema-unsupported", "Ownership schema is not supported."); }
}
void append_ownership_schema_checks(SchemaChecks& checks) {
    checks.add(std::string("SELECT c.relname,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull "
        "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='shin_server' AND c.relname IN ") + tables + " AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
        using Column = std::tuple<std::string,std::string,std::string,bool>;
        const std::set<Column> expected{
            {"identity_project_owners","project_id","text",true}, {"identity_project_owners","tenant_id","text",true},
            {"identity_workspace_owners","tenant_id","text",true}, {"identity_workspace_owners","project_id","text",true},
            {"identity_workspace_owners","workspace_id","text",true}, {"identity_workspace_owners","owner_id","text",true},
            {"identity_workspace_owners","visibility","text",true},
            {"identity_ownership_audit","id","bigint",true}, {"identity_ownership_audit","actor_id","text",true},
            {"identity_ownership_audit","tenant_id","text",true}, {"identity_ownership_audit","project_id","text",true},
            {"identity_ownership_audit","workspace_id","text",false}, {"identity_ownership_audit","action","text",true},
            {"identity_ownership_audit","created_at","timestamp with time zone",true}};
        std::set<Column> actual;
        for (const auto& row : rows) actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<bool>());
        if (actual != expected) unsupported();
    });
    checks.add(std::string("SELECT count(*)=3 AND bool_and(c.relrowsecurity AND c.relforcerowsecurity AND c.relowner=n.nspowner) "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' AND c.relname IN ") + tables,
        [](const pqxx::result& rows) { if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported(); });
    checks.add(std::string("SELECT c.relname,pg_get_constraintdef(k.oid),k.convalidated, k.condeferrable "
        "FROM pg_constraint k JOIN pg_class c ON c.oid=k.conrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='shin_server' AND k.contype<>'n' AND c.relname IN ") + tables, [](const pqxx::result& rows) {
        using Constraint = std::pair<std::string,std::string>;
        std::set<Constraint> expected, actual;
        const auto add = [&](const char* table, const char* definition) { expected.emplace(table, compact(definition)); };
        add("identity_project_owners", "PRIMARY KEY (project_id)");
        add("identity_project_owners", "UNIQUE (tenant_id, project_id)");
        add("identity_project_owners", "FOREIGN KEY (project_id) REFERENCES shin_server.projects(id) ON DELETE RESTRICT");
        add("identity_project_owners", "FOREIGN KEY (tenant_id) REFERENCES shin_server.identity_tenants(id) ON DELETE RESTRICT");
        add("identity_workspace_owners", "PRIMARY KEY (project_id, workspace_id)");
        add("identity_workspace_owners", "FOREIGN KEY (tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT");
        add("identity_workspace_owners", "FOREIGN KEY (project_id, workspace_id) REFERENCES shin_server.workspaces(project_id, id) ON DELETE RESTRICT");
        add("identity_workspace_owners", "FOREIGN KEY (tenant_id, owner_id) REFERENCES shin_server.identity_memberships(tenant_id, account_id) ON DELETE RESTRICT");
        add("identity_workspace_owners", "CHECK ((visibility = ANY (ARRAY['private'::text, 'shared'::text])))");
        add("identity_ownership_audit", "PRIMARY KEY (id)");
        add("identity_ownership_audit", "FOREIGN KEY (tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT");
        add("identity_ownership_audit", "CHECK ((actor_id ~ '^[a-z][a-z0-9-]{0,62}$'::text))");
        add("identity_ownership_audit", "CHECK ((action = ANY (ARRAY['project.bound'::text, 'workspace.bound'::text])))");
        for (const auto& row : rows) {
            if (!row[2].as<bool>() || row[3].as<bool>()) unsupported();
            actual.emplace(row[0].as<std::string>(), compact(row[1].as<std::string>()));
        }
        if (actual != expected || rows.size() != expected.size()) unsupported();
    });
    checks.add("SELECT pg_get_expr(d.adbin,d.adrelid)='''private''::text' FROM pg_attrdef d JOIN pg_attribute a "
        "ON a.attrelid=d.adrelid AND a.attnum=d.adnum WHERE d.adrelid='shin_server.identity_workspace_owners'::regclass "
        "AND a.attname='visibility'", [](const pqxx::result& rows) {
        if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported();
    });
    checks.add(std::string("SELECT c.relname,p.polname,p.polcmd,COALESCE(pg_get_expr(p.polqual,p.polrelid),''), "
        "COALESCE(pg_get_expr(p.polwithcheck,p.polrelid),''),p.polpermissive AND p.polroles=ARRAY[0]::oid[] "
        "FROM pg_policy p JOIN pg_class c ON c.oid=p.polrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='shin_server' AND c.relname IN ") + tables, [](const pqxx::result& rows) {
        using Policy = std::tuple<std::string,std::string,std::string,std::string,std::string>;
        std::set<Policy> expected, actual;
        const auto add = [&](const char* table, const char* name, const char* cmd, std::string use, std::string check) {
            expected.emplace(table, name, cmd, compact(use), compact(check));
        };
        const std::string ops = "(current_setting('shin.identity_ops'::text, true) = '1'::text)";
        const std::string tenant = "(tenant_id = current_setting('shin.identity_tenant'::text, true))";
        add("identity_project_owners", "identity_project_owners_read", "r", "((" + tenant +
            " AND (EXISTS ( SELECT 1 FROM shin_server.identity_tenants t WHERE ((t.id = identity_project_owners.tenant_id) "
            "AND (t.owner_id = current_setting('shin.identity_account'::text, true)))))) "
            "OR (project_id = ANY (string_to_array(current_setting('shin.identity_legacy_projects'::text, true), ','::text))))", "");
        add("identity_project_owners", "identity_project_owners_ops", "*", ops, ops);
        add("identity_workspace_owners", "identity_workspace_owners_read", "r", "(" + tenant +
            " AND (owner_id = current_setting('shin.identity_account'::text, true)) "
            "AND (EXISTS ( SELECT 1 FROM shin_server.identity_tenants t WHERE ((t.id = identity_workspace_owners.tenant_id) "
            "AND (t.owner_id = current_setting('shin.identity_account'::text, true))))))", "");
        add("identity_workspace_owners", "identity_workspace_owners_ops", "*", ops, ops);
        add("identity_ownership_audit", "identity_ownership_audit_read", "r", ops, "");
        add("identity_ownership_audit", "identity_ownership_audit_write", "a", "", "(" + ops +
            " AND (actor_id = current_setting('shin.identity_principal'::text, true)))");
        for (const auto& row : rows) {
            if (!row[5].as<bool>()) unsupported();
            actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                           compact(row[3].as<std::string>()), compact(row[4].as<std::string>()));
        }
        if (actual != expected) unsupported();
    });
}
} // namespace shin::server
