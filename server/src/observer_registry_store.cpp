#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/identity.h"
#include "observer_store_internal.h"

namespace shin::server {
namespace {
using namespace observer_store_detail;

const char* plan_schema(ObserverReclamationMode mode) {
    return mode == ObserverReclamationMode::discard_unconfirmed ?
        "shin.observer-discard-plan.v1" : "shin.observer-reclamation-plan.v1";
}

Json stored_retirement(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream,
                       ObserverReclamationMode mode) {
    const auto rows = tombstone_rows(tx, principal, stream);
    if (rows.empty()) throw ServiceError(404, "observer-not-found", "Observer stream was not found.");
    const auto stored = Json::parse(rows[0]["retirement"].as<std::string>());
    if (stored.at("plan").at("server_id") != read_server_identity(tx))
        throw ServiceError(409, "observer-reclamation-stale", "Retirement belongs to a previous Server identity.");
    if (stored.at("plan").at("schema_version") != plan_schema(mode))
        throw ServiceError(409, "observer-reclamation-stale", "Retirement used a different history disposition.");
    return stored;
}

Json plan_for(pqxx::transaction_base& tx, const pqxx::row_ref& row, ObserverReclamationMode mode) {
    const auto accepted = row["accepted_sequence"].as<std::int64_t>();
    const auto confirmed = row["confirmed_sequence"].as<std::int64_t>();
    const auto retired = row["retired_sequence"].as<std::int64_t>();
    const auto totals = tx.exec("SELECT count(*),COALESCE(sum(retained_bytes),0) FROM shin_server.observer_submissions WHERE stream_id=$1",
        pqxx::params{tx, row["stream_id"].as<std::string>()});
    const auto count = totals[0][0].as<std::int64_t>(), bytes = totals[0][1].as<std::int64_t>();
    if (count != accepted - retired || count > 1024 || bytes != row["retained_bytes"].as<std::int64_t>())
        throw ServiceError(503, "observer-history-inconsistent", "Retained history does not match its accounting.");
    const bool discard = mode == ObserverReclamationMode::discard_unconfirmed;
    Json reason = nullptr;
    if (!row["revoked"].as<bool>()) reason = "active-stream";
    else if (discard && confirmed == accepted) reason = "no-unconfirmed-history";
    else if (!discard && confirmed != accepted) reason = "unconfirmed-history";
    Json plan{{"schema_version", plan_schema(mode)}, {"server_id", read_server_identity(tx)},
        {"stream_id", row["stream_id"].as<std::string>()}, {"project_id", row["project_id"].as<std::string>()},
        {"workspace_id", row["workspace_id"].as<std::string>()}, {"enrollment_hash", row["enrollment_hash"].as<std::string>()},
        {"accepted_sequence", accepted}, {"confirmed_sequence", confirmed}, {"retired_sequence", retired},
        {"observed_sequence", row["observed_sequence"].as<std::int64_t>()}, {"delete_submissions", count}, {"delete_bytes", bytes},
        {"eligible", reason.is_null()}, {"blocked_reason", reason}, {"source_verified", false}, {"shared_publication_mutated", false}};
    if (discard) {
        // Review binds the complete bounded retained rows, not just their counters.
        const auto history = tx.exec("SELECT encode(sha256(convert_to(COALESCE("
            "jsonb_agg(to_jsonb(s) ORDER BY sequence),'[]'::jsonb)::text,'UTF8')),'hex') "
            "FROM shin_server.observer_submissions s WHERE stream_id=$1",
            pqxx::params{tx, row["stream_id"].as<std::string>()});
        plan["history_sha256"] = history[0][0].as<std::string>();
        plan["unconfirmed_submissions"] = accepted - confirmed;
        plan["disposition"] = "discard-unconfirmed-history";
    }
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    return plan;
}
} // namespace

Json Store::observer_reclamation(const Principal& principal, const std::string& stream, ObserverReclamationMode mode, const std::optional<ObserverScope>& scope) {
    authorize(principal, !scope && mode == ObserverReclamationMode::discard_unconfirmed);
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = find_stream_rows(tx, principal, stream, Lock::shared);
        return rows.empty() ? stored_retirement(tx, principal, stream, mode).at("plan") : plan_for(tx, rows[0], mode);
    });
}

Json Store::apply_observer_reclamation(const Principal& principal, const std::string& stream, const Json& input,
                                      ObserverReclamationMode mode, const std::optional<ObserverScope>& scope) {
    authorize(principal, !scope);
    const bool discard = mode == ObserverReclamationMode::discard_unconfirmed;
    if (discard) {
        require_fields(input, {"plan", "confirmation"});
        if (input.at("confirmation") != "discard-unconfirmed-history")
            throw ServiceError(400, "invalid-request", "Explicit unconfirmed history discard confirmation is required.");
    } else require_fields(input, {"plan"});
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        // Stream before quota, then audit, matching submission/retention writers.
        // A waiter whose row was deleted looks up the committed immutable receipt.
        const auto rows = find_stream_rows(tx, principal, stream, Lock::exclusive);
        if (rows.empty()) {
            const auto stored = stored_retirement(tx, principal, stream, mode);
            if (stored.at("plan") != input.at("plan"))
                throw ServiceError(409, "observer-reclamation-stale", "Reclamation plan does not match the retired stream.");
            return Json{{"receipt", stored.at("receipt")}, {"replayed", true}};
        }
        const auto& row = rows[0];
        const auto plan = plan_for(tx, row, mode);
        if (plan != input.at("plan"))
            throw ServiceError(409, "observer-reclamation-stale", "Reclamation plan no longer matches the stream.");
        if (!plan.at("eligible").get<bool>())
            throw ServiceError(409, "observer-reclamation-ineligible", "Stream is not eligible for the requested history disposition.");
        const auto count = plan.at("delete_submissions").get<std::int64_t>();
        const auto bytes = plan.at("delete_bytes").get<std::int64_t>();
        const auto usage = tx.exec("SELECT * FROM shin_server.observer_usage WHERE singleton FOR UPDATE");
        if (usage.size() != 1 || usage[0]["streams"].as<std::int64_t>() < 1 ||
            usage[0]["submissions"].as<std::int64_t>() < count || usage[0]["retained_bytes"].as<std::int64_t>() < bytes)
            throw ServiceError(503, "observer-history-inconsistent", "Observer quota accounting is inconsistent.");
        const auto prior = tx.exec("SELECT current_setting('shin.observer_ops',true)")[0][0].as<std::string>("");
        tx.exec("SELECT set_config('shin.observer_ops','1',true)");
        const auto project_count = tx.exec("SELECT count(*) FROM shin_server.observer_tombstones WHERE project_id=$1",
            pqxx::params{tx, row["project_id"].as<std::string>()})[0][0].as<std::int64_t>();
        tx.exec("SELECT set_config('shin.observer_ops',$1,true)", pqxx::params{tx, prior});
        if (usage[0]["tombstones"].as<std::int64_t>() >= 4096 || project_count >= 512)
            throw ServiceError(409, "observer-tombstone-capacity", "Retired identity capacity was reached; protected identities cannot be forgotten.");
        Json receipt{{"schema_version", discard ? "shin.observer-discard-receipt.v1" : "shin.observer-reclamation-receipt.v1"},
            {"server_id", plan.at("server_id")},
            {"stream_id", stream}, {"plan_id", plan.at("plan_id")}, {"principal_id", principal.id},
            {"accepted_sequence", plan.at("accepted_sequence")}, {"confirmed_sequence", plan.at("confirmed_sequence")},
            {"deleted_submissions", count}, {"deleted_bytes", bytes}, {"identity_retained", true},
            {"audit_preserved", true}, {"source_verified", false}, {"shared_publication_mutated", false}};
        if (discard) {
            receipt["history_sha256"] = plan.at("history_sha256");
            receipt["discarded_unconfirmed_submissions"] = plan.at("unconfirmed_submissions");
            receipt["disposition"] = plan.at("disposition");
            receipt["producer_durability_verified"] = false;
            receipt["archive_durability_verified"] = false;
        }
        const Json retirement{{"plan", plan}, {"receipt", receipt}};
        tx.exec("INSERT INTO shin_server.observer_tombstones(stream_id,project_id,workspace_id,principal_id,enrollment_hash,retirement,scope_tenant_id) "
            "VALUES ($1,$2,$3,$4,$5,$6::jsonb,$7)", pqxx::params{tx, stream, row["project_id"].as<std::string>(),
            row["workspace_id"].as<std::string>(), row["principal_id"].as<std::string>(), row["enrollment_hash"].as<std::string>(), retirement.dump(),
            row["scope_tenant_id"].as<std::optional<std::string>>()});
        const auto removed = tx.exec("DELETE FROM shin_server.observer_submissions WHERE stream_id=$1", pqxx::params{tx, stream});
        if (removed.affected_rows() != static_cast<pqxx::result::size_type>(count))
            throw ServiceError(503, "observer-history-inconsistent", "Retained history changed during reclamation.");
        audit(tx, principal, row, discard ? "observer.discarded" : "observer.reclaimed");
        const auto removed_stream = tx.exec("DELETE FROM shin_server.observer_streams WHERE stream_id=$1", pqxx::params{tx, stream});
        const auto updated_usage = tx.exec("UPDATE shin_server.observer_usage SET streams=streams-1,tombstones=tombstones+1,submissions=submissions-$1,retained_bytes=retained_bytes-$2 WHERE singleton",
            pqxx::params{tx, count, bytes});
        if (removed_stream.affected_rows() != 1 || updated_usage.affected_rows() != 1)
            throw ServiceError(503, "observer-history-inconsistent", "Registry changed during reclamation.");
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}
} // namespace shin::server
