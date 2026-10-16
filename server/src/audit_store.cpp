#include "shin/server/store.h"
#include "shin/server/config.h"
#include "audit_serialization.h"

namespace shin::server {
namespace {
constexpr std::size_t kMaxExportEvents = 1000;
constexpr std::size_t kMaxArchiveBytes = 1024 * 1024;

void authorize_admin(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id))
        throw ServiceError(403, "forbidden", "Administrator access is required.");
}

pqxx::result history(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT h.retired_through,h.retired_events,h.last_retention::text,u.server_id "
        "FROM shin_server.audit_history h CROSS JOIN shin_server.server_identity u WHERE h.singleton AND u.singleton");
    if (rows.size() != 1) throw ServiceError(503, "audit-history-unavailable", "Audit history is unavailable.");
    return rows;
}

Json last_receipt(const pqxx::row_ref& row) {
    return row["last_retention"].is_null() ? Json(nullptr) :
        Json::parse(row["last_retention"].as<std::string>()).at("receipt");
}

Json receipt_hash(const Json& receipt) {
    return receipt.is_null() ? Json(nullptr) : Json(digest_hex(sha256(receipt.dump())));
}

Json export_prefix(pqxx::transaction_base& tx, const pqxx::row_ref& state, std::size_t limit) {
    const auto floor = state["retired_through"].as<std::int64_t>();
    const auto rows = tx.exec(std::string("SELECT ") + audit_detail::columns +
        "FROM shin_server.audit_events WHERE id>$1 ORDER BY id ASC LIMIT $2",
        pqxx::params{tx, floor, static_cast<std::int64_t>(limit)});
    Json events = Json::array();
    for (const auto& row : rows) events.push_back(audit_detail::record(row));
    const auto through = rows.empty() ? floor : rows.back()["id"].as<std::int64_t>();
    const auto previous = last_receipt(state);
    Json archive{{"schema_version", "shin.audit-archive.v1"}, {"server_id", state["server_id"].as<std::string>()},
        {"retired_through", floor}, {"through_id", through}, {"events", std::move(events)},
        {"previous_retention_receipt", previous}, {"verification_level", "service-observed"},
        {"source_verified", false}, {"shared_publication_mutated", false}};
    const auto bytes = archive.dump();
    if (bytes.size() > kMaxArchiveBytes)
        throw ServiceError(413, "audit-export-budget", "Audit archive exceeds its byte budget. Request fewer events.");
    Json plan{{"schema_version", "shin.audit-retention-plan.v1"}, {"server_id", archive.at("server_id")},
        {"retired_through", floor}, {"through_id", through}, {"event_count", rows.size()},
        {"archive_bytes", bytes.size()}, {"archive_sha256", digest_hex(sha256(bytes))},
        {"previous_receipt_sha256", receipt_hash(previous)}};
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    return {{"schema_version", "shin.audit-export.v1"}, {"archive", std::move(archive)}, {"plan", std::move(plan)},
            {"read_only", true}, {"external_archive_verified", false}};
}

std::size_t plan_count(const Json& input) {
    require_fields(input, {"plan", "archive_saved"});
    if (input.at("archive_saved") != Json(true))
        throw ServiceError(400, "invalid-request", "Explicit external archive preservation must be declared.");
    const auto& plan = input.at("plan");
    require_fields(plan, {"schema_version", "server_id", "retired_through", "through_id", "event_count",
                         "archive_bytes", "archive_sha256", "previous_receipt_sha256", "plan_id"});
    const auto& count = plan.at("event_count");
    if (!count.is_number_integer() || count < 0 || count > kMaxExportEvents)
        throw ServiceError(400, "invalid-request", "Audit retention event count is invalid.");
    for (const auto* key : {"retired_through", "through_id", "archive_bytes"}) {
        const auto& value = plan.at(key);
        if (!value.is_number_integer() || value < 0 || value > INT64_C(9223372036854775806))
            throw ServiceError(400, "invalid-request", "Audit retention integer is invalid.");
    }
    return count.get<std::size_t>();
}
} // namespace

Json Store::audit_history(const Principal& principal) {
    authorize_admin(principal);
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT set_config('shin.audit_ops','1',true)");
        const auto rows = history(tx);
        const auto& row = rows[0];
        return Json{{"schema_version", "shin.audit-history.v1"}, {"server_id", row["server_id"].as<std::string>()},
            {"retired_through", row["retired_through"].as<std::int64_t>()},
            {"retired_events", row["retired_events"].as<std::int64_t>()}, {"last_retention_receipt", last_receipt(row)},
            {"read_only", true}, {"external_archive_verified", false}};
    });
}

Json Store::export_audit(const Principal& principal, std::size_t limit) {
    authorize_admin(principal);
    if (limit == 0 || limit > kMaxExportEvents)
        throw ServiceError(400, "invalid-limit", "Audit export limit must be between 1 and 1000.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_ops','1',true)");
        const auto rows = history(tx);
        return export_prefix(tx, rows[0], limit);
    });
}

Json Store::apply_audit_retention(const Principal& principal, const Json& input) {
    authorize_admin(principal);
    const auto count = plan_count(input);
    return run_transaction([&](pqxx::transaction_base& tx) {
        // Only audit rows and this singleton are touched while holding the writer fence.
        // Do not acquire stream/workspace locks here: their writers take this fence last.
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_ops','1',true)");
        const auto rows = history(tx);
        const auto& state = rows[0];
        if (input.at("plan").at("server_id") != state["server_id"].as<std::string>())
            throw ServiceError(409, "audit-retention-stale", "Audit plan belongs to a different Server identity.");
        if (!state["last_retention"].is_null()) {
            const auto previous = Json::parse(state["last_retention"].as<std::string>());
            if (previous.at("plan") == input.at("plan"))
                return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
        }
        const auto exported = export_prefix(tx, state, count);
        const auto& plan = exported.at("plan");
        if (input.at("plan") != plan)
            throw ServiceError(409, "audit-retention-stale", "Audit retention plan no longer matches the retained prefix.");
        if (count == 0) throw ServiceError(409, "audit-retention-empty", "No audit events are eligible for this retention plan.");
        const auto through = plan.at("through_id").get<std::int64_t>();
        const auto removed = tx.exec("DELETE FROM shin_server.audit_events WHERE id>$1 AND id<=$2",
            pqxx::params{tx, state["retired_through"].as<std::int64_t>(), through});
        if (removed.affected_rows() != static_cast<pqxx::result::size_type>(count))
            throw ServiceError(503, "audit-history-inconsistent", "Audit prefix changed unexpectedly.");
        const auto recorded = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
        Json receipt{{"schema_version", "shin.audit-retention-receipt.v1"}, {"server_id", plan.at("server_id")},
            {"plan_id", plan.at("plan_id")}, {"previous_retired_through", plan.at("retired_through")},
            {"retired_through", through}, {"deleted_events", count}, {"archive_sha256", plan.at("archive_sha256")},
            {"archive_bytes", plan.at("archive_bytes")}, {"previous_receipt_sha256", plan.at("previous_receipt_sha256")},
            {"principal_id", principal.id}, {"recorded_at", recorded[0][0].as<std::string>()},
            {"archive_saved_declared", true}, {"external_archive_verified", false}, {"shared_publication_mutated", false}};
        const Json saved{{"plan", plan}, {"receipt", receipt}};
        const auto updated = tx.exec("UPDATE shin_server.audit_history SET retired_through=$1,"
            "retired_events=retired_events+$2,last_retention=$3::jsonb WHERE singleton",
            pqxx::params{tx, through, static_cast<std::int64_t>(count), saved.dump()});
        if (updated.affected_rows() != 1)
            throw ServiceError(503, "audit-history-inconsistent", "Audit retention state is unavailable.");
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}
} // namespace shin::server
