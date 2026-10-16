#include "shin/server/store.h"
#include "preparation_access.h"
#include "shin/server/identity_directory.h"
#include "legacy_project_access.h"
#include "shin/server/publication_schema.h"
#include "publication_store_internal.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>

namespace shin::server {
namespace publication_store {
namespace {
constexpr std::size_t kPageLimit = 64;
constexpr std::int64_t kGlobalLimit = 8192;
constexpr std::int64_t kProjectLimit = 1024;

bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 63 && value.front() >= 'a' && value.front() <= 'z' &&
        std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
}

bool request_key(const std::string& value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == ':' || c == '-';
    });
}

bool request_hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    });
}

void validate_submission(std::uint64_t preparation, std::uint64_t base_revision,
                         const std::string& key, const std::string& hash) {
    validate_id(preparation);
    validate_revision(base_revision);
    if (!request_key(key) || !request_hash(hash))
        throw ServiceError(400, "invalid-request", "Invalid publication request identity.");
}

Json submission_replay(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                       const std::string& publication, std::uint64_t preparation, std::uint64_t base_revision,
                       const std::string& key, const std::string& hash, std::uint64_t epoch,
                       const std::optional<WorkspaceWriteScope>& write) {
    pqxx::params parameters{tx, principal.id, key};
    std::string replay_scope = " AND history_workspace_id IS NULL";
    if (write) {
        parameters.append(project);
        parameters.append(write->workspace.workspace_id);
        replay_scope = " AND project_id=$3 AND history_workspace_id=$4";
    }
    const auto old = tx.exec(
        "SELECT id,project_id,publication_id,preparation_id,base_revision,request_hash,history_epoch,history_workspace_id FROM shin_server.publication_proposals "
        "WHERE submitted_by=$1 AND idempotency_key=$2" + replay_scope, parameters);
    if (old.empty()) {
        if (epoch != static_cast<std::uint64_t>(admission_epoch(tx, write)))
            throw ServiceError(409, "publication-history-changed", "Publication history was retired; review the current head and admission epoch.");
        return nullptr;
    }
    const auto& row = old[0];
    if (row["project_id"].as<std::string>() != project || row["publication_id"].as<std::string>() != publication ||
        row["preparation_id"].as<std::uint64_t>() != preparation || row["base_revision"].as<std::uint64_t>() != base_revision ||
        row["request_hash"].as<std::string>() != hash || row["history_epoch"].as<std::uint64_t>() != epoch ||
        nullable_text(row["history_workspace_id"]) != (write ? Json(write->workspace.workspace_id) : Json(nullptr)))
        throw ServiceError(409, "idempotency-conflict", "The idempotency key belongs to a different publication request.");
    return {{"replayed", true}, {"proposal", select_proposal(tx, project, publication, row["id"].as<std::uint64_t>())}};
}

void validate_source_binding(const Json& source) {
    if (!source.is_object() || source.size() != 5)
        throw ServiceError(400, "invalid-request", "Invalid publication source binding.");
    for (const auto* key : {"source_route_id", "workspace_id", "source_host_id", "profile_id", "route_fingerprint"}) {
        if (!source.contains(key) || !source.at(key).is_string())
            throw ServiceError(400, "invalid-request", "Invalid publication source binding.");
        const auto value = source.at(key).get<std::string>();
        if (std::string_view(key) == "route_fingerprint" ? !request_hash(value) : !identifier(value))
            throw ServiceError(400, "invalid-request", "Invalid publication source binding.");
    }
}

// One statement observes proposal state, its immutable receipt and the same shared head.
constexpr std::string_view kDetailQuery =
    "SELECT p.id,p.project_id,p.publication_id,p.preparation_id,p.base_revision,p.base_preparation_id,p.base_withdrawal_id,p.submitted_by, "
    "to_char(p.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS proposed_at, "
    "p.source_route_id AS candidate_route,p.workspace_id AS candidate_workspace,p.source_host_id AS candidate_host, "
    "p.profile_id AS candidate_profile,p.route_fingerprint AS candidate_fingerprint, "
    "p.base_route_fingerprint AS base_fingerprint,hp.route_fingerprint AS current_fingerprint, "
    "c.artifact_snapshot_id AS candidate_snapshot,c.principal_id AS candidate_author,c.state AS candidate_state, "
    "p.base_source_route_id AS base_route,p.base_workspace_id AS base_workspace,p.base_source_host_id AS base_host,p.base_profile_id AS base_profile, "
    "b.artifact_snapshot_id AS base_snapshot, "
    "h.logical_revision AS current_revision,h.head_state,h.head_valid, "
    "h.preparation_id AS current_preparation,h.snapshot_id AS current_snapshot, "
    "hp.source_route_id AS current_route,hp.workspace_id AS current_workspace,hp.source_host_id AS current_host, "
    "hp.profile_id AS current_profile,r.decision,r.expected_revision,r.revision AS resolved_revision,r.resolved_by, "
    "to_char(r.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS resolved_at "
    "FROM shin_server.publication_proposals p "
    "JOIN shin_server.preparations c ON c.project_id=p.project_id AND c.id=p.preparation_id "
    "LEFT JOIN shin_server.preparations b ON b.project_id=p.project_id AND b.id=p.base_preparation_id "
    "CROSS JOIN publication_state h "
    "LEFT JOIN shin_server.publication_proposals hp ON hp.id=h.proposal_id AND hp.preparation_id=h.preparation_id "
    "LEFT JOIN shin_server.publication_resolutions r ON r.proposal_id=p.id "
    "WHERE p.project_id=$1 AND p.publication_id=$2 ";

// Ownership SELECTs use the transaction's account/tenant RLS context. Every
// disclosed side must be visible before LIMIT or public_detail can see the row.
constexpr std::string_view kScopedHistory =
    "AND p.workspace_id=$3 AND (p.base_revision=0 OR "
    "(p.base_withdrawal_id IS NOT NULL AND p.history_workspace_id=$3) OR "
    "(p.base_preparation_id IS NOT NULL AND EXISTS(SELECT 1 FROM shin_server.identity_workspace_owners o "
    "WHERE o.tenant_id=$4 AND o.project_id=p.project_id AND o.workspace_id=p.base_workspace_id)) OR "
    "(p.base_withdrawal_id IS NOT NULL AND EXISTS(SELECT 1 FROM shin_server.publication_withdrawals w "
    "JOIN shin_server.publication_proposals b ON b.id=w.previous_proposal_id AND b.project_id=w.project_id AND b.publication_id=w.publication_id "
    "JOIN shin_server.identity_workspace_owners o ON o.project_id=b.project_id AND o.workspace_id=b.workspace_id AND o.tenant_id=$4 "
    "WHERE w.id=p.base_withdrawal_id AND w.project_id=p.project_id AND w.publication_id=p.publication_id AND w.revision=p.base_revision))) "
    "AND (h.head_state='empty' OR (h.head_state='active' AND EXISTS(SELECT 1 FROM shin_server.identity_workspace_owners o "
    "WHERE o.tenant_id=$4 AND o.project_id=p.project_id AND o.workspace_id=hp.workspace_id)) OR "
    "(h.head_state='withdrawn' AND EXISTS(SELECT 1 FROM shin_server.publication_withdrawals w "
    "JOIN shin_server.publication_proposals b ON b.id=w.previous_proposal_id AND b.project_id=w.project_id AND b.publication_id=w.publication_id "
    "JOIN shin_server.identity_workspace_owners o ON o.project_id=b.project_id AND o.workspace_id=b.workspace_id AND o.tenant_id=$4 "
    "WHERE w.project_id=p.project_id AND w.publication_id=p.publication_id AND w.revision=h.logical_revision))) ";

void scoped_history_admission(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                              const std::string& publication, const WorkspaceReadRequest& scope) {
    if (!identifier(publication)) throw ServiceError(400, "invalid-request", "Invalid publication identity.");
    // Short metadata read: freeze directory mutations, then publication writers.
    // Neither lock is held across filesystem or Core execution.
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
    (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
    preparation_publication_sources(tx, project, publication);
}

Json public_detail(const pqxx::row_ref& row) {
    const auto base_revision = row["base_revision"].as<std::int64_t>();
    const auto revision = row["current_revision"].as<std::int64_t>();
    const auto id = row["id"].as<std::int64_t>();
    const auto preparation = row["preparation_id"].as<std::int64_t>();
    const auto project = row["project_id"].as<std::string>();
    const auto publication = row["publication_id"].as<std::string>();
    validate_head_pointer(row);
    const bool withdrawn_base = !row["base_withdrawal_id"].is_null();
    const auto current_state = row["head_state"].as<std::string>();
    if ((withdrawn_base && !row["base_preparation_id"].is_null()) ||
        (!withdrawn_base && base_revision != 0 && row["base_workspace"].is_null()) ||
        (current_state == "active" && row["current_workspace"].is_null()))
        throw ServiceError(500, "database-corrupt", "Publication source history is invalid.");
    Json resolution = nullptr;
    std::string state = base_revision == revision ? "pending" : "conflicted";
    if (!row["decision"].is_null()) {
        const auto decision = row["decision"].as<std::string>();
        state = decision == "accept" ? "accepted" : decision == "reject" ? "rejected" : "request-reprepare";
        resolution = {{"proposal_id", id}, {"project_id", project}, {"publication_id", publication},
                      {"decision", decision}, {"expected_revision", row["expected_revision"].as<std::int64_t>()},
                      {"revision", row["resolved_revision"].as<std::int64_t>()}, {"preparation_id", preparation},
                      {"created_at", row["resolved_at"].as<std::string>()}, {"resolved_by", row["resolved_by"].as<std::string>()}};
    }
    return {{"proposal", {{"id", id}, {"project_id", project}, {"publication_id", publication},
                           {"preparation_id", preparation}, {"base_revision", base_revision},
                           {"base_preparation_id", nullable_id(row["base_preparation_id"])},
                           {"base_withdrawal_id", nullable_id(row["base_withdrawal_id"])},
                           {"submitted_by", row["submitted_by"].as<std::string>()},
                           {"created_at", row["proposed_at"].as<std::string>()}, {"state", state}}},
            {"base", {{"revision", base_revision}, {"state", base_revision == 0 ? "empty" : withdrawn_base ? "withdrawn" : "active"},
                       {"preparation_id", nullable_id(row["base_preparation_id"])},
                       {"snapshot_id", nullable_text(row["base_snapshot"])},
                       {"source_route_id", nullable_text(row["base_route"])}, {"workspace_id", nullable_text(row["base_workspace"])},
                       {"source_host_id", nullable_text(row["base_host"])}, {"profile_id", nullable_text(row["base_profile"])}}},
            {"current", {{"revision", revision}, {"state", current_state}, {"preparation_id", nullable_id(row["current_preparation"])},
                          {"snapshot_id", nullable_text(row["current_snapshot"])},
                          {"source_route_id", nullable_text(row["current_route"])}, {"workspace_id", nullable_text(row["current_workspace"])},
                          {"source_host_id", nullable_text(row["current_host"])}, {"profile_id", nullable_text(row["current_profile"])}}},
            {"candidate", {{"preparation_id", preparation}, {"snapshot_id", nullable_text(row["candidate_snapshot"])},
                            {"source_route_id", row["candidate_route"].as<std::string>()}, {"workspace_id", row["candidate_workspace"].as<std::string>()},
                            {"source_host_id", row["candidate_host"].as<std::string>()}, {"profile_id", row["candidate_profile"].as<std::string>()},
                            {"candidate_author", row["candidate_author"].as<std::string>()},
                            {"state", row["candidate_state"].as<std::string>()}}},
            {"resolution", resolution}};
}
} // namespace

void validate_head_pointer(const pqxx::row_ref& row) {
    if (!row["head_valid"].as<bool>())
        throw ServiceError(500, "database-corrupt", "Publication transition history or head pointer is invalid.");
}

[[noreturn]] void conflict() {
    throw ServiceError(409, "publication-conflict", "The publication revision or proposal resolution changed.");
}

void validate_id(std::uint64_t id) {
    if (id == 0 || id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw ServiceError(400, "invalid-request", "Invalid publication proposal or preparation identity.");
}

void validate_revision(std::uint64_t revision) {
    if (revision >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw ServiceError(400, "invalid-request", "Invalid publication revision.");
}

void validate_decision(const std::string& decision) {
    if (decision != "accept" && decision != "reject" && decision != "request-reprepare")
        throw ServiceError(400, "invalid-request", "Invalid publication decision.");
}

void authorize(pqxx::transaction_base& tx, const Principal& principal,
               const std::string& project, const std::string& publication, bool admin,
               const std::optional<WorkspaceWriteScope>& write) {
    if (write) {
        authorize_workspace_write(tx, principal, project, *write);
        if (!identifier(publication)) throw ServiceError(400, "invalid-request", "Invalid publication identity.");
        return;
    }
    legacy_access::validate_principal(principal);
    if (admin) legacy_access::require_admin(principal);
    if (!identifier(project) || !identifier(publication))
        throw ServiceError(400, "invalid-request", "Invalid publication identity.");
    legacy_access::require_project_grant(principal, project);
    if (tx.exec("SELECT id FROM shin_server.projects WHERE id=$1", pqxx::params{tx, project}).empty())
        throw ServiceError(404, "project-not-found", "Project was not found.");
    legacy_preparation_scope(tx, principal);
}

void require_workspace_history(pqxx::transaction_base& tx, const std::string& project,
                               const std::string& publication, const std::optional<WorkspaceWriteScope>& write) {
    if (!write) return;
    preparation_publication_sources(tx, project, publication);
    const auto hidden = tx.exec(
        "SELECT 1 FROM shin_server.publication_proposals p WHERE p.project_id=$1 AND p.publication_id=$2 AND ("
        "p.workspace_id<>$3 OR (p.base_workspace_id IS NOT NULL AND p.base_workspace_id<>$3) OR "
        // Scoped submission verified the full lineage before persisting its
        // domain. A retired withdrawal parent must not erase that evidence.
        "(p.base_revision>0 AND p.base_workspace_id IS NULL AND "
        "NOT (p.base_withdrawal_id IS NOT NULL AND p.history_workspace_id IS NOT DISTINCT FROM $3) AND NOT EXISTS ("
        "SELECT 1 FROM shin_server.publication_withdrawals w JOIN shin_server.publication_proposals b "
        "ON b.id=w.previous_proposal_id AND b.project_id=w.project_id AND b.publication_id=w.publication_id "
        "WHERE w.id=p.base_withdrawal_id AND w.project_id=$1 AND w.publication_id=$2 AND w.revision=p.base_revision AND b.workspace_id=$3))) "
        "UNION ALL SELECT 1 FROM shin_server.publication_withdrawals w LEFT JOIN shin_server.publication_proposals p "
        "ON p.id=w.previous_proposal_id AND p.project_id=w.project_id AND p.publication_id=w.publication_id "
        "WHERE w.project_id=$1 AND w.publication_id=$2 AND p.workspace_id IS DISTINCT FROM $3 "
        "UNION ALL SELECT 1 FROM shin_server.publication_heads h LEFT JOIN shin_server.publication_proposals p "
        "ON p.id=h.proposal_id AND p.project_id=h.project_id AND p.publication_id=h.publication_id "
        "WHERE h.project_id=$1 AND h.publication_id=$2 AND p.workspace_id IS DISTINCT FROM $3 LIMIT 1",
        pqxx::params{tx, project, publication, write->workspace.workspace_id});
    if (!hidden.empty()) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
}

Json select_head(pqxx::transaction_base& tx, const std::string& project, const std::string& publication) {
    const auto rows = tx.exec(
        std::string(kHeadCte) +
        "SELECT h.logical_revision,h.head_state,h.head_valid,h.preparation_id,h.snapshot_id,h.route_fingerprint,p.route_id,p.artifact_namespace, "
        "s.source_route_id,s.workspace_id,s.source_host_id,s.profile_id,s.route_fingerprint AS proposal_fingerprint, "
        "p.artifact_key,p.artifact_bytes,p.state,p.cancel_requested,p.artifact_snapshot_id,p.route_fingerprint AS candidate_fingerprint, "
        "EXISTS(SELECT 1 FROM shin_server.preparation_retention r WHERE r.preparation_id=h.preparation_id) AS retained "
        "FROM publication_state h LEFT JOIN shin_server.preparations p "
        "ON p.project_id=h.project_id AND p.id=h.preparation_id LEFT JOIN shin_server.publication_proposals s "
        "ON s.id=h.proposal_id AND s.project_id=h.project_id AND s.publication_id=h.publication_id AND s.preparation_id=h.preparation_id",
        pqxx::params{tx, project, publication});
    Json head{{"project_id", project}, {"publication_id", publication}, {"route_id", nullptr},
              {"source_route_id", nullptr}, {"workspace_id", nullptr}, {"source_host_id", nullptr}, {"profile_id", nullptr},
              {"revision", 0}, {"state", "empty"}, {"preparation_id", nullptr},
              {"snapshot_id", nullptr}, {"route_fingerprint", nullptr}, {"artifact_namespace", nullptr},
              {"artifact_key", nullptr}, {"artifact_bytes", nullptr}};
    if (rows.size() != 1)
        throw ServiceError(500, "database-corrupt", "Publication transition history is invalid.");
    const auto& row = rows[0];
    validate_head_pointer(row);
    head["revision"] = row["logical_revision"].as<std::int64_t>();
    head["state"] = row["head_state"].as<std::string>();
    if (head.at("state") != "active") return head;
    if (row["state"].is_null() || row["state"].as<std::string>() != "prepared" ||
        row["cancel_requested"].as<bool>() || row["retained"].as<bool>() || row["artifact_bytes"].is_null() ||
        row["artifact_namespace"].is_null() || row["artifact_key"].is_null() ||
        row["workspace_id"].is_null() || nullable_text(row["source_route_id"]) != nullable_text(row["route_id"]) ||
        nullable_text(row["snapshot_id"]) != nullable_text(row["artifact_snapshot_id"]) ||
        nullable_text(row["route_fingerprint"]) != nullable_text(row["candidate_fingerprint"]) ||
        nullable_text(row["route_fingerprint"]) != nullable_text(row["proposal_fingerprint"]))
        throw ServiceError(500, "database-corrupt", "Publication generation binding is invalid.");
    head["preparation_id"] = row["preparation_id"].as<std::int64_t>();
    head["route_id"] = head["source_route_id"] = row["source_route_id"].as<std::string>();
    head["workspace_id"] = row["workspace_id"].as<std::string>();
    head["source_host_id"] = row["source_host_id"].as<std::string>();
    head["profile_id"] = row["profile_id"].as<std::string>();
    head["snapshot_id"] = row["snapshot_id"].as<std::string>();
    head["route_fingerprint"] = row["route_fingerprint"].as<std::string>();
    head["artifact_namespace"] = row["artifact_namespace"].as<std::string>();
    head["artifact_key"] = row["artifact_key"].as<std::string>();
    head["artifact_bytes"] = row["artifact_bytes"].as<std::int64_t>();
    return head;
}

Json select_proposal(pqxx::transaction_base& tx, const std::string& project,
                     const std::string& publication, std::uint64_t id, bool private_review) {
    const auto rows = tx.exec(std::string(kHeadCte) + std::string(kDetailQuery) + "AND p.id=$3", pqxx::params{tx, project, publication, id});
    if (rows.empty()) throw ServiceError(404, "publication-proposal-not-found", "Publication proposal was not found.");
    const auto& row = rows[0];
    auto detail = public_detail(row);
    if (!private_review) return detail;
    return {{"detail", detail}, {"fingerprints", {{"base", nullable_text(row["base_fingerprint"])},
        {"current", nullable_text(row["current_fingerprint"])}, {"candidate", nullable_text(row["candidate_fingerprint"])}}}};
}

Json replay_resolution(const Json& detail, const std::string& decision, std::uint64_t expected_revision) {
    const auto& receipt = detail.at("resolution");
    if (!receipt.is_null()) {
        if (receipt.at("decision") != decision || receipt.at("expected_revision") != expected_revision) conflict();
        return {{"replayed", true}, {"receipt", receipt}};
    }
    if (detail.at("current").at("revision") != expected_revision) conflict();
    return nullptr;
}

void audit(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
           std::uint64_t preparation, std::uint64_t proposal, const std::string& action) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");
    tx.exec("INSERT INTO shin_server.audit_events(principal_id,action,project_id,preparation_id,publication_proposal_id) "
            "VALUES($1,$2,$3,$4,$5)", pqxx::params{tx, principal.id, action, project, preparation, proposal});
}
} // namespace publication_store

Json Store::publication_head(const Principal& principal, const std::string& project, const std::string& publication,
                             bool include_admission) {
    return run_transaction([&](pqxx::transaction_base& tx) {
        publication_store::authorize(tx, principal, project, publication, false);
        if (include_admission)
            tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        auto head = publication_store::select_head(tx, project, publication);
        // Internal snapshot selection must not change when unrelated history is retired.
        if (include_admission) head["history_epoch"] = publication_store::admission_epoch(tx);
        return head;
    });
}

Json Store::scoped_publication_head(const Principal& principal, const std::string& project,
                                    const std::string& publication, const WorkspaceReadRequest& scope) {
    if (!valid_identifier(publication)) throw ServiceError(400, "invalid-request", "Invalid publication identity.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
        // Pin the pointer across the scoped existence check and normal integrity
        // validation. Publication mutation/retention already use this writer lock.
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        const auto visible = tx.exec("SELECT 1 FROM shin_server.publication_heads h "
            "JOIN shin_server.publication_proposals p ON p.id=h.proposal_id AND p.project_id=h.project_id "
            "AND p.publication_id=h.publication_id AND p.preparation_id=h.preparation_id "
            "WHERE h.project_id=$1 AND h.publication_id=$2 AND p.workspace_id=$3",
            pqxx::params{tx, project, publication, scope.workspace_id});
        if (visible.empty()) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        preparation_publication_sources(tx, project, publication);
        auto head = publication_store::select_head(tx, project, publication);
        if (head.at("state") != "active" || head.at("workspace_id") != scope.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        return head;
    });
}

Json Store::publication_proposal(const Principal& principal, const std::string& project,
                                const std::string& publication, std::uint64_t id) {
    publication_store::validate_id(id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        publication_store::authorize(tx, principal, project, publication, false);
        return publication_store::select_proposal(tx, project, publication, id);
    });
}

Json Store::publication_review_input(const Principal& principal, const std::string& project,
                                    const std::string& publication, std::uint64_t id) {
    publication_store::validate_id(id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        publication_store::authorize(tx, principal, project, publication, false);
        return publication_store::select_proposal(tx, project, publication, id, true);
    });
}

Json Store::scoped_publication_proposal(const Principal& principal, const std::string& project,
                                        const std::string& publication, std::uint64_t id, const WorkspaceReadRequest& scope) {
    publication_store::validate_id(id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        publication_store::scoped_history_admission(tx, principal, project, publication, scope);
        const auto rows = tx.exec(std::string(publication_store::kHeadCte) + std::string(publication_store::kDetailQuery) +
            std::string(publication_store::kScopedHistory) + "AND p.id=$5",
            pqxx::params{tx, project, publication, scope.workspace_id, scope.tenant_id, id});
        if (rows.empty()) throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        return publication_store::public_detail(rows[0]);
    });
}

Json Store::scoped_publication_proposals(const Principal& principal, const std::string& project,
                                         const std::string& publication, std::uint64_t after, std::size_t limit,
                                         const WorkspaceReadRequest& scope) {
    using namespace publication_store;
    if (after > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw ServiceError(400, "invalid-cursor", "The pagination cursor is invalid.");
    if (limit == 0 || limit > kPageLimit) throw ServiceError(400, "invalid-limit", "The requested page size is invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        scoped_history_admission(tx, principal, project, publication, scope);
        const auto page_limit = static_cast<pqxx::result::size_type>(limit);
        const auto rows = tx.exec(std::string(kHeadCte) + std::string(kDetailQuery) + std::string(kScopedHistory) +
            "AND p.id>$5 ORDER BY p.id LIMIT $6", pqxx::params{tx, project, publication, scope.workspace_id, scope.tenant_id, after, page_limit + 1});
        Json items = Json::array();
        for (pqxx::result::size_type index = 0; index < std::min(page_limit, rows.size()); ++index) items.push_back(public_detail(rows[index]));
        const Json next = rows.size() > page_limit ? items.back().at("proposal").at("id") : Json(nullptr);
        // The global retention epoch/count would reveal unrelated project activity.
        return Json{{"tenant_id", scope.tenant_id}, {"project_id", project}, {"workspace_id", scope.workspace_id},
                    {"items", std::move(items)}, {"next_after", next}};
    });
}

Json Store::list_publication_proposals(const Principal& principal, const std::string& project,
                                      const std::string& publication, std::uint64_t after, std::size_t limit) {
    using namespace publication_store;
    if (after > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw ServiceError(400, "invalid-cursor", "The pagination cursor is invalid.");
    if (limit == 0 || limit > kPageLimit) throw ServiceError(400, "invalid-limit", "The requested page size is invalid.");
    const auto page_limit = static_cast<pqxx::result::size_type>(limit);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, false);
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        const auto rows = tx.exec(std::string(kHeadCte) + std::string(kDetailQuery) + "AND p.id>$3 ORDER BY p.id LIMIT $4",
                                  pqxx::params{tx, project, publication, after, page_limit + 1});
        Json items = Json::array();
        for (pqxx::result::size_type index = 0; index < std::min(page_limit, rows.size()); ++index)
            items.push_back(public_detail(rows[index]));
        const Json next = rows.size() > page_limit ? items.back().at("proposal").at("id") : Json(nullptr);
        const auto epoch = admission_epoch(tx);
        // Maintenance pages include scoped records too. Their legacy admission
        // clock cannot certify completeness after any scoped project retirement.
        const auto retired = tx.exec("SELECT EXISTS(SELECT 1 FROM shin_server.workspace_publication_history "
            "WHERE project_id=$1 AND history_epoch>0)", pqxx::params{tx, project});
        return Json{{"items", items}, {"next_after", next}, {"history_epoch", epoch},
                    {"history_complete", epoch == 0 && !retired[0][0].as<bool>()}};
    });
}

Json Store::publication_submission_replay(const Principal& principal, const std::string& project,
                                          const std::string& publication, std::uint64_t preparation,
                                          std::uint64_t base_revision, const std::string& key, const std::string& hash,
                                          std::uint64_t epoch, const std::optional<WorkspaceWriteScope>& write) {
    using namespace publication_store;
    validate_submission(preparation, base_revision, key, hash);
    validate_revision(epoch);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, false, write);
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        require_workspace_history(tx, project, publication, write);
        const auto replay = submission_replay(tx, principal, project, publication, preparation, base_revision, key, hash, epoch, write);
        if (!replay.is_null()) return replay;
        if (write) require_scoped_candidate(tx, principal, preparation, *write);
        const auto candidate = tx.exec("SELECT principal_id FROM shin_server.preparations WHERE project_id=$1 AND id=$2",
                                       pqxx::params{tx, project, preparation});
        if (candidate.empty() || (!principal.admin && candidate[0][0].as<std::string>() != principal.id))
            throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
        return Json(nullptr);
    });
}

Json Store::submit_publication_proposal(const Principal& principal, const std::string& project,
                                       const std::string& publication, std::uint64_t preparation,
                                       std::uint64_t base_revision, const std::string& key, const std::string& hash,
                                       const Json& source_binding, std::uint64_t epoch,
                                       const std::optional<WorkspaceWriteScope>& write) {
    using namespace publication_store;
    validate_submission(preparation, base_revision, key, hash);
    validate_revision(epoch);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, false, write);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.publication-writers.v1'))");
        require_workspace_history(tx, project, publication, write);
        const auto replay = submission_replay(tx, principal, project, publication, preparation, base_revision, key, hash, epoch, write);
        if (!replay.is_null()) return replay;
        if (write) require_scoped_candidate(tx, principal, preparation, *write);
        validate_source_binding(source_binding);
        const auto source_route = source_binding.at("source_route_id").get<std::string>();
        const auto workspace = source_binding.at("workspace_id").get<std::string>();
        if (write && workspace != write->workspace.workspace_id)
            throw ServiceError(404, "identity-not-found", "Identity resource was not found.");
        const auto source_host = source_binding.at("source_host_id").get<std::string>();
        const auto profile = source_binding.at("profile_id").get<std::string>();
        const auto fingerprint = source_binding.at("route_fingerprint").get<std::string>();
        const auto workspace_rows = tx.exec(
            "SELECT source_host_id,source_kind FROM shin_server.workspaces WHERE project_id=$1 AND id=$2",
            pqxx::params{tx, project, workspace});
        if (source_host != source_host_id_ || workspace_rows.size() != 1 ||
            workspace_rows[0]["source_host_id"].as<std::string>() != source_host ||
            workspace_rows[0]["source_kind"].as<std::string>() != "local")
            throw ServiceError(409, "preparation-route-changed", "Candidate source binding changed.");
        const auto counts = tx.exec(
            "SELECT count(*) AS global_count,count(*) FILTER(WHERE project_id=$1) AS project_count "
            "FROM shin_server.publication_proposals", pqxx::params{tx, project});
        if (counts[0]["global_count"].as<std::int64_t>() >= kGlobalLimit || counts[0]["project_count"].as<std::int64_t>() >= kProjectLimit)
            throw ServiceError(409, "publication-proposal-capacity", "Publication proposal history is full.");
        if (base_revision > select_head(tx, project, publication).at("revision").get<std::uint64_t>()) conflict();
        std::optional<std::int64_t> base;
        std::optional<std::int64_t> base_withdrawal;
        std::optional<std::string> base_route, base_workspace, base_host, base_profile, base_fingerprint;
        if (base_revision != 0) {
            const auto rows = tx.exec(
                "SELECT r.preparation_id,NULL::bigint AS withdrawal_id,p.source_route_id,p.workspace_id,p.source_host_id,p.profile_id,p.route_fingerprint "
                "FROM shin_server.publication_resolutions r JOIN shin_server.publication_proposals p ON p.id=r.proposal_id "
                "WHERE r.project_id=$1 AND r.publication_id=$2 AND r.decision='accept' AND r.revision=$3 "
                "UNION ALL SELECT NULL::bigint,id,NULL::text,NULL::text,NULL::text,NULL::text,NULL::text FROM shin_server.publication_withdrawals "
                "WHERE project_id=$1 AND publication_id=$2 AND revision=$3",
                pqxx::params{tx, project, publication, base_revision});
            if (rows.size() != 1) conflict();
            if (!rows[0]["preparation_id"].is_null()) base = rows[0]["preparation_id"].as<std::int64_t>();
            if (!rows[0]["withdrawal_id"].is_null()) base_withdrawal = rows[0]["withdrawal_id"].as<std::int64_t>();
            if (base) {
                base_route = rows[0]["source_route_id"].as<std::string>();
                base_workspace = rows[0]["workspace_id"].as<std::string>();
                base_host = rows[0]["source_host_id"].as<std::string>();
                base_profile = rows[0]["profile_id"].as<std::string>();
                base_fingerprint = rows[0]["route_fingerprint"].as<std::string>();
            }
        }
        // Retention takes the same row locks, then reads publication references without a writer lock.
        const bool source_read_only = write.has_value() || !principal.admin;
        if (source_read_only) lock_preparation_inputs(tx, project, preparation, base);
        const auto candidates = tx.exec(
            std::string("SELECT id,principal_id,route_id,route_fingerprint,state,cancel_requested,artifact_namespace,artifact_key,artifact_snapshot_id,artifact_bytes ") +
            "FROM shin_server.preparations WHERE project_id=$1 AND (id=$2 OR id=$3) ORDER BY id" + (source_read_only ? "" : " FOR UPDATE"),
            pqxx::params{tx, project, preparation, base});
        const pqxx::result::size_type expected_rows = base && *base != static_cast<std::int64_t>(preparation) ? 2 : 1;
        if (candidates.size() != expected_rows)
            throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
        for (const auto& row : candidates) {
            if (row["id"].as<std::uint64_t>() == preparation && !principal.admin && row["principal_id"].as<std::string>() != principal.id)
                throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
            if (row["id"].as<std::uint64_t>() == preparation &&
                (row["route_id"].as<std::string>() != source_route || row["route_fingerprint"].as<std::string>() != fingerprint))
                throw ServiceError(409, "preparation-route-changed", "Candidate source binding changed.");
            if (row["state"].as<std::string>() != "prepared" || row["cancel_requested"].as<bool>() ||
                row["artifact_namespace"].is_null() || row["artifact_key"].is_null() ||
                row["artifact_snapshot_id"].is_null() || row["artifact_bytes"].is_null())
                throw ServiceError(409, "publication-candidate-unavailable", "Publication requires retained prepared candidates.");
        }
        if (!tx.exec("SELECT 1 FROM shin_server.preparation_retention WHERE preparation_id=$1 OR preparation_id=$2",
                     pqxx::params{tx, preparation, base}).empty())
            throw ServiceError(409, "candidate-retention-pending", "Candidate is reserved for retention.");
        if (write) tx.exec("INSERT INTO shin_server.workspace_publication_history(project_id,workspace_id) VALUES($1,$2) "
            "ON CONFLICT (project_id,workspace_id) DO NOTHING", pqxx::params{tx, project, write->workspace.workspace_id});
        const auto inserted = tx.exec(
            "INSERT INTO shin_server.publication_proposals(project_id,publication_id,preparation_id,base_revision,base_preparation_id, "
            "submitted_by,idempotency_key,request_hash,source_route_id,workspace_id,source_host_id,profile_id,route_fingerprint,base_withdrawal_id, "
            "history_epoch,base_source_route_id,base_workspace_id,base_source_host_id,base_profile_id,base_route_fingerprint,history_workspace_id) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21) RETURNING id",
            pqxx::params{tx, project, publication, preparation, base_revision, base, principal.id, key, hash,
                         source_route, workspace, source_host, profile, fingerprint, base_withdrawal, epoch,
                         base_route, base_workspace, base_host, base_profile, base_fingerprint,
                         write ? std::optional(write->workspace.workspace_id) : std::nullopt});
        const auto proposal = inserted[0][0].as<std::uint64_t>();
        audit(tx, principal, project, preparation, proposal, "publication.proposed");
        return Json{{"replayed", false}, {"proposal", select_proposal(tx, project, publication, proposal)}};
    });
}

bool publication_references_preparation(pqxx::transaction_base& tx, std::uint64_t id) {
    publication_store::validate_id(id);
    const auto rows = tx.exec(
        "SELECT EXISTS(SELECT 1 FROM shin_server.publication_heads WHERE preparation_id=$1) OR "
        "EXISTS(SELECT 1 FROM shin_server.publication_proposals p WHERE (p.preparation_id=$1 OR p.base_preparation_id=$1) "
        "AND NOT EXISTS(SELECT 1 FROM shin_server.publication_resolutions r WHERE r.proposal_id=p.id))",
        pqxx::params{tx, id});
    return rows[0][0].as<bool>();
}
} // namespace shin::server
