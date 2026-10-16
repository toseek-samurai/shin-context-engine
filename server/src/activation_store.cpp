#include "shin/server/store.h"
#include "preparation_access.h"
#include "route_access.h"
#include "shin/server/identity_directory.h"
#include "legacy_project_access.h"
#include "shin/server/publication_schema.h"

#include <pqxx/pqxx>
#include <algorithm>
#include <limits>
#include <optional>

namespace shin::server {
namespace {
constexpr auto kMaxRevision = std::numeric_limits<std::int64_t>::max() - 1;
constexpr std::int64_t kMaxHistory = 65536;

[[noreturn]] void conflict() {
    throw ServiceError(409, "activation-conflict", "The expected route revision or activation target changed.");
}

bool identifier(const std::string& text) {
    return !text.empty() && text.size() <= 63 && text.front() >= 'a' && text.front() <= 'z' &&
        std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
}

void authorize(pqxx::transaction_base& tx, const Principal& principal,
               const std::string& project, const std::string& route, bool admin,
               const std::optional<WorkspaceWriteScope>& write = std::nullopt) {
    if (write) {
        authorize_workspace_write(tx, principal, project, *write);
        if (write->route_id != route || !identifier(route))
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        select_content_route(tx, route);
        return;
    }
    legacy_access::validate_principal(principal);
    if (admin) legacy_access::require_admin(principal);
    if (!identifier(project) || !identifier(route))
        throw ServiceError(400, "invalid-request", "Invalid route identity.");
    legacy_access::require_project_grant(principal, project);
    if (tx.exec("SELECT id FROM shin_server.projects WHERE id=$1", pqxx::params{tx, project}).empty())
        throw ServiceError(404, "project-not-found", "Project was not found.");
    legacy_preparation_scope(tx, principal);
}

std::optional<std::int64_t> target_id(const Json& value) {
    if (value.is_null()) return {};
    if (!value.is_number_integer() || value < 1 || value > std::numeric_limits<std::int64_t>::max())
        throw ServiceError(400, "invalid-request", "Invalid preparation identity.");
    return value.get<std::int64_t>();
}

void validate_revision(std::uint64_t revision) {
    if (revision > static_cast<std::uint64_t>(kMaxRevision))
        throw ServiceError(400, "invalid-request", "Invalid expected route revision.");
}

template<class Field> Json nullable(const Field& field) {
    return field.is_null() ? Json(nullptr) : Json(field.template as<std::string>());
}

Json select_active(pqxx::transaction_base& tx, const std::string& project, const std::string& route, bool scoped = false) {
    if (scoped) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.activation-writers.v1'))");
        preparation_route_source(tx, project, route);
    }
    const auto rows = tx.exec(
        "SELECT a.revision,a.preparation_id,a.snapshot_id,a.route_fingerprint, "
        "p.artifact_namespace,p.artifact_key,p.artifact_bytes,p.state,p.artifact_snapshot_id,p.route_fingerprint AS job_fingerprint, "
        "EXISTS(SELECT 1 FROM shin_server.preparation_retention r WHERE r.preparation_id=a.preparation_id) AS retained "
        "FROM shin_server.active_routes a LEFT JOIN shin_server.preparations p ON p.id=a.preparation_id "
        "WHERE a.project_id=$1 AND a.route_id=$2", pqxx::params{tx, project, route});
    Json result{{"project_id", project}, {"route_id", route}, {"revision", 0}, {"preparation_id", nullptr},
                {"snapshot_id", nullptr}, {"route_fingerprint", nullptr}, {"artifact_namespace", nullptr},
                {"artifact_key", nullptr}, {"artifact_bytes", nullptr}};
    if (rows.empty()) return result;
    const auto& row = rows[0];
    result["revision"] = row["revision"].as<std::int64_t>();
    if (row["preparation_id"].is_null()) return result;
    if (row["state"].is_null() || row["state"].as<std::string>() != "prepared" || row["retained"].as<bool>() ||
        nullable(row["snapshot_id"]) != nullable(row["artifact_snapshot_id"]) || row["artifact_bytes"].is_null() ||
        row["artifact_namespace"].is_null() || row["artifact_key"].is_null() ||
        nullable(row["route_fingerprint"]) != nullable(row["job_fingerprint"]))
        throw ServiceError(500, "database-corrupt", "Active generation binding is invalid.");
    result["preparation_id"] = row["preparation_id"].as<std::int64_t>();
    result["snapshot_id"] = nullable(row["snapshot_id"]);
    result["route_fingerprint"] = nullable(row["route_fingerprint"]);
    result["artifact_namespace"] = nullable(row["artifact_namespace"]);
    result["artifact_key"] = nullable(row["artifact_key"]);
    result["artifact_bytes"] = row["artifact_bytes"].as<std::int64_t>();
    return result;
}

Json replay(pqxx::transaction_base& tx, const std::string& project, const std::string& route,
            std::int64_t previous, const std::optional<std::int64_t>& target) {
    const auto rows = tx.exec(
        "SELECT preparation_id,snapshot_id,revision, "
        "to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at "
        "FROM shin_server.activation_receipts WHERE project_id=$1 AND route_id=$2 AND previous_revision=$3",
        pqxx::params{tx, project, route, previous});
    if (rows.empty()) return nullptr;
    const auto& row = rows[0];
    const auto actual = row["preparation_id"].is_null() ? std::optional<std::int64_t>{} : row["preparation_id"].as<std::int64_t>();
    if (actual != target) conflict();
    return {{"project_id", project}, {"route_id", route}, {"previous_revision", previous},
            {"revision", row["revision"].as<std::int64_t>()},
            {"preparation_id", actual ? Json(*actual) : Json(nullptr)},
            {"snapshot_id", nullable(row["snapshot_id"])}, {"created_at", row["created_at"].as<std::string>()}};
}
} // namespace

Json Store::active_route(const Principal& principal, const std::string& project, const std::string& route) {
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, route, false);
        return select_active(tx, project, route);
    });
}

Json Store::scoped_active_route(const Principal& principal, const RetrievalRoute& route, const WorkspaceReadRequest& scope) {
    if (!identifier(route.id)) throw ServiceError(400, "invalid-request", "Invalid route identity.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, route.project_id, scope.workspace_id);
        if (route.workspace_id != scope.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        return select_active(tx, route.project_id, route.id, true);
    });
}

Json Store::activation_replay(const Principal& principal, const std::string& project, const std::string& route,
                              std::uint64_t expected_revision, const Json& preparation_id,
                              const std::optional<WorkspaceWriteScope>& write) {
    validate_revision(expected_revision);
    const auto target = target_id(preparation_id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, route, true, write);
        if (write) {
            tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.activation-writers.v1'))");
            if (target) require_scoped_candidate(tx, principal, *target, *write);
        }
        const auto receipt = replay(tx, project, route, static_cast<std::int64_t>(expected_revision), target);
        if (!receipt.is_null()) return Json{{"replayed", true}, {"receipt", receipt}};
        if (select_active(tx, project, route, write.has_value()).at("revision") != expected_revision) conflict();
        return Json(nullptr);
    });
}

Json Store::commit_activation(const Principal& principal, const std::string& project, const std::string& route,
                              std::uint64_t expected_revision, const Json& verified_input,
                              const std::optional<WorkspaceWriteScope>& write) {
    validate_revision(expected_revision);
    const auto target = verified_input.is_null() ? std::optional<std::int64_t>{} : target_id(verified_input.at("job").at("id"));
    const auto previous = static_cast<std::int64_t>(expected_revision);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, route, true, write);
        // One short transaction owns CAS, receipt allocation, and the global history bound.
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.activation-writers.v1'))");
        if (write && target) require_scoped_candidate(tx, principal, *target, *write);
        const auto old_receipt = replay(tx, project, route, previous, target);
        if (!old_receipt.is_null()) return Json{{"replayed", true}, {"receipt", old_receipt}};
        const auto active = select_active(tx, project, route, write.has_value());
        if (active.at("revision") != previous) conflict();
        if (activation_history_size(tx) >= kMaxHistory)
            throw ServiceError(409, "activation-history-capacity", "Activation history is full.");

        std::optional<std::string> snapshot;
        std::optional<std::string> fingerprint;
        if (target) {
            const auto rows = tx.exec("SELECT * FROM shin_server.preparations WHERE id=$1 FOR UPDATE", pqxx::params{tx, *target});
            if (rows.empty()) throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
            const auto& row = rows[0];
            const auto& job = verified_input.at("job");
            if (row["project_id"].as<std::string>() != project || row["route_id"].as<std::string>() != route ||
                row["state"].as<std::string>() != "prepared" || row["cancel_requested"].as<bool>() ||
                row["expected_route_revision"].as<std::int64_t>() != previous ||
                row["expected_snapshot_id"].as<std::string>() != (active.at("snapshot_id").is_null() ? "" : active.at("snapshot_id").get<std::string>()) ||
                nullable(row["artifact_snapshot_id"]) != job.at("artifact_snapshot_id") ||
                row["artifact_bytes"].is_null() || row["artifact_bytes"].as<std::int64_t>() != job.at("artifact_bytes") ||
                nullable(row["route_fingerprint"]) != verified_input.at("route_fingerprint") ||
                nullable(row["artifact_namespace"]) != verified_input.at("artifact_namespace") ||
                nullable(row["artifact_key"]) != verified_input.at("artifact_key")) conflict();
            if (!tx.exec("SELECT 1 FROM shin_server.preparation_retention WHERE preparation_id=$1", pqxx::params{tx, *target}).empty())
                throw ServiceError(409, "candidate-retention-pending", "Candidate is reserved for retention.");
            snapshot = row["artifact_snapshot_id"].as<std::string>();
            fingerprint = row["route_fingerprint"].as<std::string>();
        }
        tx.exec("INSERT INTO shin_server.active_routes(project_id,route_id,revision,preparation_id,snapshot_id,route_fingerprint,scope_workspace_id) "
                "VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT(project_id,route_id) DO UPDATE SET "
                "revision=EXCLUDED.revision,preparation_id=EXCLUDED.preparation_id,snapshot_id=EXCLUDED.snapshot_id,route_fingerprint=EXCLUDED.route_fingerprint,"
                "scope_workspace_id=COALESCE(active_routes.scope_workspace_id,EXCLUDED.scope_workspace_id)",
                pqxx::params{tx, project, route, previous + 1, target, snapshot, fingerprint,
                    write ? std::optional(write->workspace.workspace_id) : std::nullopt});
        tx.exec("INSERT INTO shin_server.activation_receipts(project_id,route_id,previous_revision,revision,preparation_id,snapshot_id,principal_id,scope_workspace_id) "
                "SELECT $1,$2,$3,$4,$5,$6,$7,scope_workspace_id FROM shin_server.active_routes WHERE project_id=$1 AND route_id=$2",
                pqxx::params{tx, project, route, previous, previous + 1, target, snapshot, principal.id});
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");
        tx.exec("INSERT INTO shin_server.audit_events(principal_id,action,project_id,preparation_id,activation_route_id,activation_previous_revision) "
                "VALUES($1,$2,$3,$4,$5,$6)",
                pqxx::params{tx, principal.id, target ? "generation.activated" : "generation.deactivated", project, target, route, previous});
        return Json{{"replayed", false}, {"receipt", replay(tx, project, route, previous, target)}};
    });
}

bool Store::preparation_is_active(const Principal& principal, std::uint64_t id,
                                 const std::optional<WorkspaceWriteScope>& write) {
    if (!write) (void)get_preparation(principal, id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        if (write) authorize_preparation_cleanup(tx, principal, id, *write);
        return Json(preparation_has_active_route(tx, id) ||
                    publication_references_preparation(tx, id));
    }).get<bool>();
}

} // namespace shin::server
