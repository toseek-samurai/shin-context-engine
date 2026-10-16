#include "shin/server/identity_directory.h"
#include "shin/server/config.h"
#include "shin/server/store.h"
#include <pqxx/pqxx>

namespace shin::server {
namespace {
void identifier(const std::string& value) {
    if (!valid_identifier(value)) throw ServiceError(400, "invalid-request", "Invalid ownership identity.");
}
void page(const std::string& after, std::size_t limit) {
    if ((!after.empty() && !valid_identifier(after)) || limit == 0 || limit > 100)
        throw ServiceError(400, "invalid-request", "Invalid page boundary.");
}
[[noreturn]] void missing() { throw ServiceError(404, "identity-not-found", "Identity resource was not found."); }
[[noreturn]] void conflict() { throw ServiceError(409, "identity-conflict", "Ownership binding already differs."); }
void binding_write(pqxx::transaction_base& tx, const Principal& actor, const std::string& tenant) {
    if (!actor.admin) throw ServiceError(403, "forbidden", "Explicit maintenance ownership mapping is required.");
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.identity_directory.v1'))");
    authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
}
void project_scope(pqxx::transaction_base& tx, const std::string& tenant, const std::string& project) {
    identifier(project);
    if (tx.exec("SELECT project_id FROM shin_server.identity_project_owners WHERE tenant_id=$1 AND project_id=$2",
                pqxx::params{tx, tenant, project}).empty()) missing();
}
void audit(pqxx::transaction_base& tx, const Principal& actor, const std::string& tenant,
           const std::string& project, const std::string& workspace, const char* action) {
    tx.exec("INSERT INTO shin_server.identity_ownership_audit(actor_id,tenant_id,project_id,workspace_id,action) "
            "VALUES($1,$2,$3,NULLIF($4,''),$5)", pqxx::params{tx, actor.id, tenant, project, workspace, action});
}
Json project_json(const std::string& tenant, const std::string& project) {
    return {{"tenant_id", tenant}, {"project_id", project}, {"content_access", "scoped-routes-only"}};
}
template<class Row> Json workspace_json(const Row& row) {
    return {{"tenant_id", row["tenant_id"].template as<std::string>()},
        {"project_id", row["project_id"].template as<std::string>()},
        {"workspace_id", row["workspace_id"].template as<std::string>()},
        {"owner_id", row["owner_id"].template as<std::string>()},
        {"visibility", row["visibility"].template as<std::string>()}, {"content_access", "scoped-routes-only"}};
}
}

Json authorize_identity_workspace(pqxx::transaction_base& tx, const Principal& actor,
                                  const std::string& tenant, const std::string& project,
                                  const std::string& workspace) {
    identifier(project); identifier(workspace);
    authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
    const auto rows = tx.exec("SELECT * FROM shin_server.identity_workspace_owners "
        "WHERE tenant_id=$1 AND project_id=$2 AND workspace_id=$3", pqxx::params{tx, tenant, project, workspace});
    if (rows.empty()) missing();
    tx.exec("SELECT set_config('shin.content_project',$1,true), set_config('shin.content_workspace',$2,true)",
            pqxx::params{tx, project, workspace});
    auto ownership = workspace_json(rows[0]);
    // Keep the historical presentation field stable in persisted admission receipts.
    // Public metadata below advertises the new HTTP subset without invalidating jobs.
    ownership["content_access"] = "not-enabled";
    return ownership;
}

Json Store::identity_bind_project(const Principal& actor, const std::string& tenant, const Json& input) {
    if (!actor.admin) throw ServiceError(403, "forbidden", "Explicit maintenance ownership mapping is required.");
    require_fields(input, {"project_id"});
    const auto project = require_text(input, "project_id", 63); identifier(project);
    return run_transaction([&](auto& tx) {
        binding_write(tx, actor, tenant);
        const auto existing = tx.exec("SELECT tenant_id FROM shin_server.identity_project_owners WHERE project_id=$1", pqxx::params{tx, project});
        if (!existing.empty()) {
            if (existing[0]["tenant_id"].template as<std::string>() != tenant) conflict();
            return Json{{"project", project_json(tenant, project)}, {"replayed", true}};
        }
        if (tx.exec("SELECT id FROM shin_server.projects WHERE id=$1", pqxx::params{tx, project}).empty()) missing();
        tx.exec("INSERT INTO shin_server.identity_project_owners(project_id,tenant_id) VALUES($1,$2)", pqxx::params{tx, project, tenant});
        audit(tx, actor, tenant, project, "", "project.bound");
        return Json{{"project", project_json(tenant, project)}, {"replayed", false}};
    });
}

Json Store::identity_project(const Principal& actor, const std::string& tenant, const std::string& project) {
    return run_transaction([&](auto& tx) {
        authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
        project_scope(tx, tenant, project);
        return project_json(tenant, project);
    });
}

Json Store::identity_projects(const Principal& actor, const std::string& tenant, const std::string& after, std::size_t limit) {
    page(after, limit);
    return run_transaction([&](auto& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
        const auto rows = tx.exec("SELECT p.id,p.display_name FROM shin_server.identity_project_owners o "
            "JOIN shin_server.projects p ON p.id=o.project_id WHERE o.tenant_id=$1 AND p.id>$2 ORDER BY p.id LIMIT $3",
            pqxx::params{tx, tenant, after, limit + 1});
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);
        Json items = Json::array();
        for (pqxx::result::size_type i = 0; i < rows.size() && i < result_limit; ++i) {
            auto item = project_json(tenant, rows[i]["id"].template as<std::string>());
            item["display_name"] = rows[i]["display_name"].template as<std::string>();
            items.push_back(std::move(item));
        }
        return Json{{"items", items}, {"next_cursor", rows.size() > result_limit ? items.back()["project_id"] : Json(nullptr)}};
    });
}

Json Store::identity_workspaces(const Principal& actor, const std::string& tenant, const std::string& project,
                                const std::string& after, std::size_t limit) {
    page(after, limit);
    return run_transaction([&](auto& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        authorize_identity_tenant(tx, actor, tenant, IdentityOperation::tenant_read);
        project_scope(tx, tenant, project);
        const auto rows = tx.exec("SELECT o.tenant_id,o.project_id,o.workspace_id,o.owner_id,o.visibility,w.display_name "
            "FROM shin_server.identity_workspace_owners o JOIN shin_server.workspaces w ON w.project_id=o.project_id AND w.id=o.workspace_id "
            "WHERE o.tenant_id=$1 AND o.project_id=$2 AND o.workspace_id>$3 ORDER BY o.workspace_id LIMIT $4",
            pqxx::params{tx, tenant, project, after, limit + 1});
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);
        Json items = Json::array();
        for (pqxx::result::size_type i = 0; i < rows.size() && i < result_limit; ++i) {
            auto item = workspace_json(rows[i]);
            item["display_name"] = rows[i]["display_name"].template as<std::string>();
            items.push_back(std::move(item));
        }
        return Json{{"items", items}, {"next_cursor", rows.size() > result_limit ? items.back()["workspace_id"] : Json(nullptr)}};
    });
}

Json Store::identity_bind_workspace(const Principal& actor, const std::string& tenant,
                                    const std::string& project, const Json& input) {
    if (!actor.admin) throw ServiceError(403, "forbidden", "Explicit maintenance ownership mapping is required.");
    auto body = input;
    if (body.is_object() && !body.contains("visibility")) body["visibility"] = "private";
    require_fields(body, {"workspace_id", "owner_id", "visibility"});
    const auto workspace = require_text(body, "workspace_id", 63), owner = require_text(body, "owner_id", 63);
    const auto visibility = require_text(body, "visibility", 16);
    identifier(workspace); identifier(owner);
    if (visibility != "private" && visibility != "shared") throw ServiceError(400, "invalid-request", "Invalid workspace visibility.");
    if (visibility != "private") throw ServiceError(403, "sharing-disabled", "This Server release supports private workspaces only.");
    return run_transaction([&](auto& tx) {
        binding_write(tx, actor, tenant); project_scope(tx, tenant, project);
        if (tx.exec("SELECT id FROM shin_server.identity_tenants WHERE id=$1 AND owner_id=$2",
                    pqxx::params{tx, tenant, owner}).empty())
            throw ServiceError(403, "sharing-disabled", "Workspace owner must own the personal space.");
        const auto member = tx.exec("SELECT a.id FROM shin_server.identity_accounts a JOIN shin_server.identity_memberships m ON m.account_id=a.id "
            "WHERE a.id=$1 AND a.enabled AND m.tenant_id=$2 AND m.enabled", pqxx::params{tx, owner, tenant});
        if (member.empty()) missing();
        const auto existing = tx.exec("SELECT * FROM shin_server.identity_workspace_owners WHERE project_id=$1 AND workspace_id=$2",
                                       pqxx::params{tx, project, workspace});
        if (!existing.empty()) {
            if (existing[0]["tenant_id"].template as<std::string>() != tenant || existing[0]["owner_id"].template as<std::string>() != owner ||
                existing[0]["visibility"].template as<std::string>() != visibility) conflict();
            return Json{{"workspace", workspace_json(existing[0])}, {"replayed", true}};
        }
        if (tx.exec("SELECT id FROM shin_server.workspaces WHERE project_id=$1 AND id=$2", pqxx::params{tx, project, workspace}).empty()) missing();
        const auto rows = tx.exec("INSERT INTO shin_server.identity_workspace_owners(tenant_id,project_id,workspace_id,owner_id,visibility) "
            "VALUES($1,$2,$3,$4,$5) RETURNING *", pqxx::params{tx, tenant, project, workspace, owner, visibility});
        audit(tx, actor, tenant, project, workspace, "workspace.bound");
        return Json{{"workspace", workspace_json(rows[0])}, {"replayed", false}};
    });
}

Json Store::identity_workspace(const Principal& actor, const std::string& tenant, const std::string& project, const std::string& workspace) {
    return run_transaction([&](auto& tx) {
        auto result = authorize_identity_workspace(tx, actor, tenant, project, workspace);
        result["content_access"] = "scoped-routes-only";
        return result;
    });
}
} // namespace shin::server
