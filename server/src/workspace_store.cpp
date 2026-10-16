#include "shin/server/store.h"
#include "legacy_project_access.h"
#include "shin/server/config.h"
#include "shin/server/identity_directory.h"

#include <pqxx/pqxx>
#include <algorithm>
#include <optional>

namespace shin::server {
namespace {
constexpr std::int64_t kMaxWorkspaces = 4096;
constexpr std::int64_t kMaxProjectWorkspaces = 128;
constexpr std::int64_t kMaxRequests = 65536;
constexpr auto kWorkspaceColumns =
    "project_id,id,source_host_id,display_name,root,root_identity,source_kind,revision,"
    "to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at";

bool bounded_text(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum && text.find('\0') == std::string_view::npos;
}

void authorize(const Principal& principal, const std::string& project, bool admin) {
    legacy_access::validate_principal(principal);
    if (admin) legacy_access::require_admin(principal);
    if (!valid_identifier(project))
        throw ServiceError(400, "invalid-request", "Invalid project identity.");
    legacy_access::require_project_grant(principal, project);
}

void require_project(pqxx::transaction_base& tx, const std::string& project) {
    if (tx.exec("SELECT id FROM shin_server.projects WHERE id=$1", pqxx::params{tx, project}).empty())
        throw ServiceError(404, "project-not-found", "Project was not found.");
}

template<class Row> Json workspace_json(const Row& row) {
    return {{"project_id", row["project_id"].template as<std::string>()},
            {"id", row["id"].template as<std::string>()},
            {"source_host_id", row["source_host_id"].template as<std::string>()},
            {"display_name", row["display_name"].template as<std::string>()},
            {"root", row["root"].is_null() ? Json(nullptr) : Json(row["root"].template as<std::string>())},
            {"root_identity", row["root_identity"].is_null() ? Json(nullptr) : Json(row["root_identity"].template as<std::string>())},
            {"source_kind", row["source_kind"].template as<std::string>()},
            {"created_at", row["created_at"].template as<std::string>()},
            {"revision", row["revision"].template as<std::int64_t>()}};
}

void validate_request_identity(std::string_view key, std::string_view hash) {
    if (!bounded_text(key, 128) || !std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == ':' || c == '-';
        }))
        throw ServiceError(400, "invalid-idempotency-key", "A valid Idempotency-Key is required.");
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw ServiceError(400, "invalid-request", "Invalid request identity.");
}

Json select_replay(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                   std::string_view key, std::string_view hash) {
    const auto prior = tx.exec("SELECT request_hash,project_id,response::text FROM shin_server.workspace_requests "
                               "WHERE principal_id=$1 AND project_id=$2 AND key=$3",
                               pqxx::params{tx, principal.id, project, std::string(key)});
    if (prior.empty()) return nullptr;
    if (prior[0]["request_hash"].as<std::string>() != hash || prior[0]["project_id"].as<std::string>() != project)
        throw ServiceError(409, "idempotency-conflict", "The idempotency key was used for a different request.");
    const auto workspace = Json::parse(prior[0]["response"].as<std::string>());
    if (!workspace.is_object() || workspace.value("project_id", "") != project)
        throw ServiceError(500, "database-corrupt", "Stored workspace receipt is invalid.");
    return Json{{"workspace", workspace}, {"replayed", true}};
}
} // namespace

Json Store::workspace_registration_replay(const Principal& principal, const std::string& project,
                                          std::string_view key, std::string_view hash) {
    authorize(principal, project, true);
    validate_request_identity(key, hash);
    return run_transaction([&](pqxx::transaction_base& tx) {
        require_project(tx, project);
        return select_replay(tx, principal, project, key, hash);
    });
}

Json Store::register_workspace(const Principal& principal, const std::string& project,
                               const WorkspaceInput& input, std::string_view key, std::string_view hash) {
    authorize(principal, project, true);
    validate_request_identity(key, hash);
    if (!valid_identifier(input.id) || !valid_identifier(input.source_host_id) ||
        !bounded_text(input.display_name, 120))
        throw ServiceError(400, "invalid-request", "Invalid workspace binding.");
    if (input.observer_source ? (!input.root.empty() || !input.root_identity.empty()) :
        (!bounded_text(input.root, 4096) || input.root.front() != '/' || !bounded_text(input.root_identity, 4096)))
        throw ServiceError(400, "invalid-request", "Invalid workspace source shape.");
    if (!input.observer_source && input.source_host_id != source_host_id_)
        throw ServiceError(409, "workspace-host-mismatch", "Workspace belongs to a different source host.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        require_project(tx, project);
        // Registration, bounded history and duplicate delivery share one short write transaction.
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.workspace-writers.v1'))");
        const auto prior = select_replay(tx, principal, project, key, hash);
        if (!prior.is_null()) return prior;
        const auto counts = tx.exec("SELECT (SELECT count(*) FROM shin_server.workspaces) AS total, "
                                     "(SELECT count(*) FROM shin_server.workspaces WHERE project_id=$1) AS owned, "
                                     "(SELECT count(*) FROM shin_server.workspace_requests) AS requests", pqxx::params{tx, project});
        if (counts[0]["total"].as<std::int64_t>() >= kMaxWorkspaces ||
            counts[0]["owned"].as<std::int64_t>() >= kMaxProjectWorkspaces ||
            counts[0]["requests"].as<std::int64_t>() >= kMaxRequests)
            throw ServiceError(409, "workspace-capacity", "Workspace registry capacity was reached.");
        const auto rows = tx.exec(std::string("INSERT INTO shin_server.workspaces ") +
            "(project_id,id,source_host_id,display_name,root,root_identity,source_kind) VALUES ($1,$2,$3,$4,$5,$6,$7) "
            "ON CONFLICT DO NOTHING RETURNING " + kWorkspaceColumns,
            pqxx::params{tx, project, input.id, input.source_host_id, input.display_name,
                input.observer_source ? std::nullopt : std::optional(input.root),
                input.observer_source ? std::nullopt : std::optional(input.root_identity),
                input.observer_source ? "observer" : "local"});
        if (rows.empty())
            throw ServiceError(409, "workspace-conflict", "Workspace identity or local root is already registered.");
        const auto workspace = workspace_json(rows[0]);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");
        tx.exec("INSERT INTO shin_server.audit_events(principal_id,action,project_id,workspace_id) "
                "VALUES ($1,'workspace.registered',$2,$3)", pqxx::params{tx, principal.id, project, input.id});
        tx.exec("INSERT INTO shin_server.workspace_requests(principal_id,key,request_hash,project_id,workspace_id,response) "
                "VALUES ($1,$2,$3,$4,$5,$6::jsonb)",
                pqxx::params{tx, principal.id, std::string(key), std::string(hash), project, input.id, workspace.dump()});
        return Json{{"workspace", workspace}, {"replayed", false}};
    });
}

Json Store::get_workspace(const Principal& principal, const std::string& project, const std::string& id) {
    authorize(principal, project, false);
    if (!valid_identifier(id)) throw ServiceError(400, "invalid-request", "Invalid workspace identity.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        require_project(tx, project);
        const auto rows = tx.exec(std::string("SELECT ") + kWorkspaceColumns +
                                 " FROM shin_server.workspaces WHERE project_id=$1 AND id=$2", pqxx::params{tx, project, id});
        if (rows.empty()) throw ServiceError(404, "workspace-not-found", "Workspace was not found.");
        return workspace_json(rows[0]);
    });
}

Json Store::list_workspaces(const Principal& principal, const std::string& project,
                            const std::string& after, std::size_t limit) {
    authorize(principal, project, false);
    if (!after.empty() && !valid_identifier(after))
        throw ServiceError(400, "invalid-cursor", "The pagination cursor is invalid.");
    if (limit < 1 || limit > 100) throw ServiceError(400, "invalid-limit", "The requested page size is invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        require_project(tx, project);
        const auto rows = tx.exec(std::string("SELECT ") + kWorkspaceColumns +
            " FROM shin_server.workspaces WHERE project_id=$1 AND id>$2 ORDER BY id ASC LIMIT $3",
            pqxx::params{tx, project, after, static_cast<std::int64_t>(limit + 1)});
        Json items = Json::array();
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);
        for (pqxx::result::size_type index = 0; index < std::min(result_limit, rows.size()); ++index)
            items.push_back(workspace_json(rows[index]));
        Json cursor = rows.size() > result_limit ? items.back().at("id") : Json(nullptr);
        return Json{{"items", std::move(items)}, {"next_cursor", std::move(cursor)}};
    });
}

Json scoped_workspace_snapshot(pqxx::transaction_base& tx, const Principal& principal,
                               const std::string& project, const WorkspaceReadRequest& scope) {
    const auto ownership = authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
    Json authorization{{"ownership", ownership}, {"principal_id", principal.id},
        {"maintenance", principal.admin}, {"account_id", nullptr}, {"account_revision", nullptr}, {"membership_revision", nullptr}};
    if (!principal.admin) {
        const auto rows = tx.exec("SELECT a.id,a.revision,m.revision AS membership_revision "
            "FROM shin_server.identity_accounts a JOIN shin_server.identity_memberships m ON m.account_id=a.id "
            "WHERE a.principal_id=$1 AND a.enabled AND m.tenant_id=$2 AND m.enabled",
            pqxx::params{tx, principal.id, scope.tenant_id});
        if (rows.empty()) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        authorization["account_id"] = rows[0]["id"].as<std::string>();
        authorization["account_revision"] = rows[0]["revision"].as<std::int64_t>();
        authorization["membership_revision"] = rows[0]["membership_revision"].as<std::int64_t>();
    }
    const auto rows = tx.exec(std::string("SELECT ") + kWorkspaceColumns +
        " FROM shin_server.workspaces WHERE project_id=$1 AND id=$2", pqxx::params{tx, project, scope.workspace_id});
    if (rows.empty()) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
    return Json{{"workspace", workspace_json(rows[0])}, {"authorization", authorization}};
}

Json Store::scoped_workspace(const Principal& principal, const std::string& project, const WorkspaceReadRequest& scope) {
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        return scoped_workspace_snapshot(tx, principal, project, scope);
    });
}

Json workspace_owner_admission(pqxx::transaction_base& tx, const Principal& principal,
                               const std::string& project, const WorkspaceReadRequest& scope) {
    const auto admission = scoped_workspace_snapshot(tx, principal, project, scope);
    const auto& authority = admission.at("authorization");
    if (!principal.admin && authority.at("account_id") != authority.at("ownership").at("owner_id"))
        throw ServiceError(403, "workspace-owner-required", "Mutation requires workspace ownership.");
    return admission;
}

void authorize_workspace_write(pqxx::transaction_base& tx, const Principal& principal,
                               const std::string& project, const WorkspaceWriteScope& scope) {
    if (scope.project_id != project || !scope.admission.is_object())
        throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
    if (workspace_owner_admission(tx, principal, project, scope.workspace) != scope.admission)
        throw ServiceError(409, "workspace-write-scope-changed", "Workspace authority changed; start a new request.");
}

void require_scoped_candidate(pqxx::transaction_base& tx, const Principal& principal,
                              std::uint64_t id, const WorkspaceWriteScope& scope) {
    const auto rows = tx.exec("SELECT 1 FROM shin_server.preparations WHERE id=$1 AND project_id=$2 "
        "AND principal_id=$3 AND workspace_admission=$4::jsonb",
        pqxx::params{tx, id, scope.project_id, principal.id, scope.admission.dump()});
    if (rows.empty()) throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
}

void authorize_preparation_cleanup(pqxx::transaction_base& tx, const Principal& principal,
                                   std::uint64_t id, const WorkspaceWriteScope& scope) {
    authorize_workspace_write(tx, principal, scope.project_id, scope);
    const auto rows = tx.exec("SELECT 1 FROM shin_server.preparations WHERE id=$1 AND project_id=$2 "
        "AND (principal_id=$3 OR $4) "
        "AND scope_tenant_id=$5 AND scope_workspace_id=$6",
        pqxx::params{tx, id, scope.project_id, principal.id, principal.admin, scope.workspace.tenant_id, scope.workspace.workspace_id});
    if (rows.empty()) throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
}

} // namespace shin::server
