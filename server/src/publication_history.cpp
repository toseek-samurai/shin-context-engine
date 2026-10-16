#include "shin/server/store.h"
#include "shin/server/config.h"
#include "publication_store_internal.h"

#include <map>

namespace shin::server {
namespace {
constexpr std::size_t kMaxRetire = 256;

void admin(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id))
        throw ServiceError(403, "forbidden", "Administrator access is required.");
}

void authorize(pqxx::transaction_base& tx, const Principal& principal,
               const std::optional<WorkspaceWriteScope>& write) {
    if (write) authorize_workspace_write(tx, principal, write->project_id, *write);
    else admin(principal);
}

std::optional<std::string> project_key(const std::optional<WorkspaceWriteScope>& write) {
    return write ? std::optional(write->project_id) : std::nullopt;
}
std::optional<std::string> workspace_key(const std::optional<WorkspaceWriteScope>& write) {
    return write ? std::optional(write->workspace.workspace_id) : std::nullopt;
}

constexpr std::string_view kDomain =
    "(($1::text IS NULL AND history_workspace_id IS NULL) OR "
    "(project_id=$1 AND history_workspace_id=$2::text))";

void lock_history(pqxx::transaction_base& tx, bool apply) {
    tx.exec(apply ? "SELECT pg_advisory_xact_lock(hashtext('shin_server.publication-writers.v1'))" :
                    "SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
    // Publication writers already take the audit fence last. Never reverse this order.
    // Only reference IDs enter the inventory; audit payloads are never returned by this path.
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_ops','1',true)");
}

pqxx::result history(pqxx::transaction_base& tx, const std::optional<WorkspaceWriteScope>& write) {
    if (write) {
        const auto rows = tx.exec("SELECT COALESCE(h.history_epoch,0) AS history_epoch,"
            "COALESCE(h.retired_proposals,0) AS retired_proposals,h.last_retention,i.server_id "
            "FROM shin_server.server_identity i LEFT JOIN shin_server.workspace_publication_history h "
            "ON h.project_id=$1 AND h.workspace_id=$2 WHERE i.singleton",
            pqxx::params{tx, write->project_id, write->workspace.workspace_id});
        if (rows.size() != 1)
            throw ServiceError(503, "publication-history-unavailable", "Publication history state is unavailable.");
        return rows;
    }
    const auto rows = tx.exec("SELECT h.*,i.server_id FROM shin_server.publication_history h "
        "CROSS JOIN shin_server.server_identity i WHERE h.singleton AND i.singleton");
    if (rows.size() != 1)
        throw ServiceError(503, "publication-history-unavailable", "Publication history state is unavailable.");
    return rows;
}

// Unique protection sets avoid per-proposal rescans and duplicate inventory rows.
// Preserve both logical transition anchors used by kHeadCte, even for withdrawn publications.
constexpr std::string_view kInventory =
    "WITH anchors AS (SELECT proposal_id FROM (SELECT DISTINCT ON(project_id,publication_id) proposal_id "
    "FROM shin_server.publication_resolutions WHERE decision='accept' ORDER BY project_id,publication_id,revision DESC) a "
    "UNION SELECT previous_proposal_id FROM (SELECT DISTINCT ON(project_id,publication_id) previous_proposal_id "
    "FROM shin_server.publication_withdrawals ORDER BY project_id,publication_id,revision DESC) w "
    "UNION SELECT proposal_id FROM shin_server.publication_heads), inventory AS ("
    "SELECT p.id,p.project_id,p.history_workspace_id,r.proposal_id IS NULL AS unresolved, "
    "a.proposal_id IS NOT NULL AS anchored,e.publication_proposal_id IS NOT NULL AS audited "
    "FROM shin_server.publication_proposals p LEFT JOIN shin_server.publication_resolutions r ON r.proposal_id=p.id "
    "LEFT JOIN anchors a ON a.proposal_id=p.id "
    "LEFT JOIN (SELECT DISTINCT publication_proposal_id FROM shin_server.audit_events "
    "WHERE publication_proposal_id IS NOT NULL) e ON e.publication_proposal_id=p.id) ";

Json counts(pqxx::transaction_base& tx, const std::optional<WorkspaceWriteScope>& write) {
    const auto rows = tx.exec(std::string(kInventory) +
        "SELECT count(*) AS retained,count(*) FILTER(WHERE unresolved) AS unresolved,"
        "count(*) FILTER(WHERE anchored) AS anchored,count(*) FILTER(WHERE audited) AS audit_pinned,"
        "count(*) FILTER(WHERE NOT unresolved AND NOT anchored AND NOT audited) AS eligible FROM inventory WHERE " +
        std::string(kDomain), pqxx::params{tx, project_key(write), workspace_key(write)});
    const auto& row = rows[0];
    Json result;
    for (const auto* key : {"retained", "unresolved", "anchored", "audit_pinned", "eligible"})
        result[key] = row[key].as<std::int64_t>();
    return result;
}

Json previous_receipt(const pqxx::row_ref& state) {
    return state["last_retention"].is_null() ? Json(nullptr) :
        Json::parse(state["last_retention"].as<std::string>()).at("receipt");
}

Json make_plan(pqxx::transaction_base& tx, const pqxx::row_ref& state, std::size_t limit,
               const std::optional<WorkspaceWriteScope>& write) {
    const auto rows = tx.exec(std::string(kInventory) +
        "SELECT p.id,p.project_id,to_jsonb(p)::text AS proposal,to_jsonb(r)::text AS resolution,"
        "COALESCE((SELECT jsonb_agg(to_jsonb(w) ORDER BY w.id) FROM shin_server.publication_withdrawals w "
        "WHERE w.previous_proposal_id=p.id),'[]'::jsonb)::text AS withdrawals "
        "FROM (SELECT * FROM inventory WHERE " + std::string(kDomain) + ") i "
        "JOIN shin_server.publication_proposals p ON p.id=i.id "
        "JOIN shin_server.publication_resolutions r ON r.proposal_id=p.id "
        "WHERE NOT i.unresolved AND NOT i.anchored AND NOT i.audited ORDER BY p.id LIMIT $3",
        pqxx::params{tx, project_key(write), workspace_key(write), static_cast<std::int64_t>(limit)});
    Json ids = Json::array(), records = Json::array(), projects = Json::array();
    std::map<std::string, std::size_t> affected;
    std::size_t withdrawals = 0;
    for (const auto& row : rows) {
        ids.push_back(row["id"].as<std::int64_t>());
        ++affected[row["project_id"].as<std::string>()];
        const auto removed = Json::parse(row["withdrawals"].as<std::string>());
        withdrawals += removed.size();
        records.push_back({{"proposal", Json::parse(row["proposal"].as<std::string>())},
                           {"resolution", Json::parse(row["resolution"].as<std::string>())}, {"withdrawals", removed}});
    }
    for (const auto& [project, count] : affected) projects.push_back({{"project_id", project}, {"delete_count", count}});
    const auto previous = previous_receipt(state);
    Json plan{{"schema_version", "shin.publication-history-plan.v1"}, {"scope", write ? "workspace" : "legacy-publications"},
        {"server_id", state["server_id"].as<std::string>()}, {"history_epoch", state["history_epoch"].as<std::int64_t>()},
        {"limit", limit}, {"proposal_ids", std::move(ids)}, {"delete_count", rows.size()},
        {"withdrawal_count", withdrawals}, {"affected_projects", std::move(projects)},
        {"counts", counts(tx, write)}, {"records_sha256", digest_hex(sha256(records.dump()))},
        {"previous_receipt_sha256", previous.is_null() ? Json(nullptr) : Json(digest_hex(sha256(previous.dump())))}};
    if (write) {
        plan["tenant_id"] = write->workspace.tenant_id;
        plan["project_id"] = write->project_id;
        plan["workspace_id"] = write->workspace.workspace_id;
    }
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    if (plan.dump().size() > 32768)
        throw ServiceError(413, "publication-history-budget", "Request a smaller publication history plan.");
    return plan;
}

std::size_t validate_input(const Json& input, const std::optional<WorkspaceWriteScope>& write) {
    require_fields(input, {"plan", "discard_confirmed"});
    if (input.at("discard_confirmed") != Json(true))
        throw ServiceError(400, "invalid-request", "Explicit publication history discard confirmation is required.");
    const auto& plan = input.at("plan");
    if (write) require_fields(plan, {"schema_version", "scope", "server_id", "history_epoch", "limit", "proposal_ids",
        "delete_count", "withdrawal_count", "affected_projects", "counts", "records_sha256", "previous_receipt_sha256", "plan_id",
        "tenant_id", "project_id", "workspace_id"});
    else require_fields(plan, {"schema_version", "scope", "server_id", "history_epoch", "limit", "proposal_ids",
        "delete_count", "withdrawal_count", "affected_projects", "counts", "records_sha256", "previous_receipt_sha256", "plan_id"});
    const auto& limit = plan.at("limit");
    if (!limit.is_number_integer() || limit < 1 || limit > kMaxRetire || plan.dump().size() > 32768)
        throw ServiceError(400, "invalid-request", "Publication history plan bounds are invalid.");
    return limit.get<std::size_t>();
}
} // namespace

std::int64_t publication_store::admission_epoch(pqxx::transaction_base& tx, const std::optional<WorkspaceWriteScope>& write) {
    if (write) return history(tx, write)[0]["history_epoch"].as<std::int64_t>();
    const auto rows = tx.exec("SELECT history_epoch FROM shin_server.publication_history WHERE singleton");
    if (rows.size() != 1)
        throw ServiceError(503, "publication-history-unavailable", "Publication admission epoch is unavailable.");
    return rows[0][0].as<std::int64_t>();
}

Json Store::publication_history(const Principal& principal, const std::optional<WorkspaceWriteScope>& write) {
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, write);
        lock_history(tx, false);
        const auto rows = history(tx, write);
        const auto& state = rows[0];
        Json result{{"schema_version", "shin.publication-history.v1"}, {"scope", write ? "workspace" : "legacy-publications"},
            {"server_id", state["server_id"].as<std::string>()}, {"history_epoch", state["history_epoch"].as<std::int64_t>()},
            {"retired_proposals", state["retired_proposals"].as<std::int64_t>()},
            {"counts", counts(tx, write)}, {"last_retention_receipt", previous_receipt(state)},
            {"history_complete", state["history_epoch"].as<std::int64_t>() == 0}, {"read_only", true}};
        if (write) {
            result["tenant_id"] = write->workspace.tenant_id;
            result["project_id"] = write->project_id;
            result["workspace_id"] = write->workspace.workspace_id;
        }
        return result;
    });
}

Json Store::plan_publication_retention(const Principal& principal, std::size_t limit,
                                       const std::optional<WorkspaceWriteScope>& write) {
    if (limit == 0 || limit > kMaxRetire)
        throw ServiceError(400, "invalid-limit", "Publication history plan limit must be between 1 and 256.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, write);
        lock_history(tx, false);
        const auto rows = history(tx, write);
        return make_plan(tx, rows[0], limit, write);
    });
}

Json Store::apply_publication_retention(const Principal& principal, const Json& input,
                                        const std::optional<WorkspaceWriteScope>& write) {
    const auto limit = validate_input(input, write);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, write);
        lock_history(tx, true);
        const auto rows = history(tx, write);
        const auto& state = rows[0];
        const auto& requested = input.at("plan");
        if (requested.at("server_id") != state["server_id"].as<std::string>())
            throw ServiceError(409, "publication-history-stale", "Publication history deployment identity changed.");
        if (!state["last_retention"].is_null()) {
            const auto previous = Json::parse(state["last_retention"].as<std::string>());
            if (previous.at("plan") == requested) return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
        }
        const auto plan = make_plan(tx, state, limit, write);
        if (plan != requested)
            throw ServiceError(409, "publication-history-stale", "Publication history changed; inspect a fresh plan.");
        const auto count = plan.at("delete_count").get<std::int64_t>();
        if (count == 0) throw ServiceError(409, "publication-history-empty", "No unprotected resolved proposals can retire.");
        const auto epoch = state["history_epoch"].as<std::int64_t>();
        if (epoch >= INT64_MAX - 1 || state["retired_proposals"].as<std::int64_t>() > INT64_MAX - count)
            throw ServiceError(409, "publication-history-capacity", "Publication history counters cannot advance.");
        const auto ids = plan.at("proposal_ids").dump();
        const auto withdrawals = tx.exec("DELETE FROM shin_server.publication_withdrawals WHERE previous_proposal_id IN "
            "(SELECT value::bigint FROM jsonb_array_elements_text($1::jsonb))", pqxx::params{tx, ids});
        const auto resolutions = tx.exec("DELETE FROM shin_server.publication_resolutions WHERE proposal_id IN "
            "(SELECT value::bigint FROM jsonb_array_elements_text($1::jsonb))", pqxx::params{tx, ids});
        const auto proposals = tx.exec("DELETE FROM shin_server.publication_proposals WHERE id IN "
            "(SELECT value::bigint FROM jsonb_array_elements_text($1::jsonb))", pqxx::params{tx, ids});
        if (withdrawals.affected_rows() != plan.at("withdrawal_count").get<pqxx::result::size_type>() ||
            resolutions.affected_rows() != count || proposals.affected_rows() != count)
            throw ServiceError(503, "publication-history-inconsistent", "Publication retirement count changed.");
        const auto clock = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
        Json receipt{{"schema_version", "shin.publication-history-receipt.v1"}, {"server_id", plan.at("server_id")},
            {"plan_id", plan.at("plan_id")}, {"previous_history_epoch", epoch}, {"history_epoch", epoch + 1},
            {"deleted_proposals", count}, {"deleted_withdrawals", plan.at("withdrawal_count")},
            {"previous_receipt_sha256", plan.at("previous_receipt_sha256")}, {"principal_id", principal.id},
            {"recorded_at", clock[0][0].as<std::string>()}, {"discard_confirmed", true},
            {"external_archive_verified", false}, {"source_mutated", false}, {"publication_head_mutated", false}};
        if (write) {
            receipt["scope"] = "workspace";
            receipt["tenant_id"] = write->workspace.tenant_id;
            receipt["project_id"] = write->project_id;
            receipt["workspace_id"] = write->workspace.workspace_id;
        }
        const Json saved{{"plan", plan}, {"receipt", receipt}};
        const auto changed = write ? tx.exec("UPDATE shin_server.workspace_publication_history SET history_epoch=$1,"
            "retired_proposals=retired_proposals+$2,last_retention=$3::jsonb WHERE project_id=$4 AND workspace_id=$5",
            pqxx::params{tx, epoch + 1, count, saved.dump(), write->project_id, write->workspace.workspace_id}) :
            tx.exec("UPDATE shin_server.publication_history SET history_epoch=$1,"
            "retired_proposals=retired_proposals+$2,last_retention=$3::jsonb WHERE singleton",
            pqxx::params{tx, epoch + 1, count, saved.dump()});
        if (changed.affected_rows() != 1)
            throw ServiceError(503, "publication-history-unavailable", "Publication retirement state disappeared.");
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}
} // namespace shin::server
