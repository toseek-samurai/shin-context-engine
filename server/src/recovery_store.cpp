#include "shin/server/recovery.h"
#include "shin/server/config.h"
#include "shin/server/store.h"
#include "shin/server/identity.h"

#include <pqxx/pqxx>
#include <set>
#include <tuple>

namespace shin::server {
namespace {
bool identity(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 32 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void validate_request(const Json& request, const std::string& expected, IdentityMaintenance mode) {
    require_fields(request, {"schema_version", "operation_id", "expected_server_id", "reason_code", "service_fenced"});
    const bool retire = mode == IdentityMaintenance::observer_retirement;
    if (request.at("schema_version") != (retire ? "shin.observer-retirement-request.v1" : "shin.server-recovery-request.v1") ||
        !identity(request.at("operation_id")) || !identity(request.at("expected_server_id")) ||
        request.at("expected_server_id") != expected || request.at("service_fenced") != Json(true) ||
        (retire ? request.at("reason_code") != "retired-identity-capacity" :
         request.at("reason_code") != "backup-restore" && request.at("reason_code") != "clone"))
        throw ServiceError(400, "invalid-recovery-request", "Recovery identities or operator declarations are invalid.");
}

std::string successor_id(const Json& request, IdentityMaintenance mode) {
    const std::string domain = mode == IdentityMaintenance::observer_retirement ?
        "shin.observer-retirement-identity.v1\n" : "shin.server-recovery-identity.v1\n";
    return digest_hex(sha256(domain + request.at("expected_server_id").get<std::string>() +
                             "\n" + request.at("operation_id").get<std::string>())).substr(0, 32);
}

Json stored_recovery(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT last_recovery::text FROM shin_server.server_recovery WHERE singleton");
    if (rows.size() != 1) throw ServiceError(503, "recovery-state-unavailable", "Recovery state is unavailable.");
    return rows[0][0].is_null() ? Json(nullptr) : Json::parse(rows[0][0].as<std::string>());
}

Json plan_for(pqxx::transaction_base& tx, const Json& request, const Json& previous, IdentityMaintenance mode) {
    const auto usage = tx.exec("SELECT server_id,streams,submissions,retained_bytes,tombstones FROM shin_server.observer_usage JOIN shin_server.server_identity USING(singleton) WHERE singleton");
    if (usage.size() != 1 || usage[0]["server_id"].as<std::string>() != request.at("expected_server_id").get<std::string>())
        throw ServiceError(409, "recovery-identity-mismatch", "Restore identity does not match the requested branch of history.");
    const auto streams = tx.exec("SELECT to_jsonb(s)::text AS state,revoked FROM shin_server.observer_streams s ORDER BY stream_id LIMIT 1025");
    if (streams.size() > 1024 || streams.size() != usage[0]["streams"].as<std::int64_t>())
        throw ServiceError(503, "recovery-state-inconsistent", "Observer registry accounting is inconsistent.");
    Json states = Json::array();
    std::size_t active = 0;
    for (const auto& row : streams) {
        states.push_back(Json::parse(row["state"].as<std::string>()));
        if (!row["revoked"].as<bool>()) ++active;
    }
    const auto tombstones = tx.exec("SELECT stream_id,project_id,workspace_id,principal_id,enrollment_hash,"
        "encode(sha256(convert_to(retirement::text,'UTF8')),'hex') AS retirement_sha256 "
        "FROM shin_server.observer_tombstones ORDER BY stream_id LIMIT 4097");
    if (tombstones.size() > 4096 || tombstones.size() != usage[0]["tombstones"].as<std::int64_t>())
        throw ServiceError(503, "recovery-state-inconsistent", "Retired identity accounting is inconsistent.");
    Json retired_states = Json::array();
    for (const auto& row : tombstones) {
        Json state = Json::array();
        for (const auto& value : row) state.push_back(value.as<std::string>());
        retired_states.push_back(std::move(state));
    }
    const auto audit = tx.exec("SELECT to_jsonb(h)::text FROM shin_server.audit_history h WHERE singleton");
    if (audit.size() != 1) throw ServiceError(503, "recovery-state-unavailable", "Audit retention state is unavailable.");
    const auto previous_receipt = previous.is_null() ? Json(nullptr) : previous.at("receipt");
    const auto successor = successor_id(request, mode);
    if (successor == request.at("expected_server_id").get<std::string>())
        throw ServiceError(409, "recovery-identity-conflict", "Choose a new recovery operation identity.");
    Json plan{{"schema_version", mode == IdentityMaintenance::observer_retirement ?
        "shin.observer-retirement-plan.v1" : "shin.server-recovery-plan.v2"}, {"request", request}, {"new_server_id", successor},
        {"stream_count", streams.size()}, {"active_stream_count", active},
        {"observer_state_sha256", digest_hex(sha256(states.dump()))},
        {"tombstone_count", tombstones.size()}, {"tombstone_state_sha256", digest_hex(sha256(retired_states.dump()))},
        {"retained_submissions", usage[0]["submissions"].as<std::int64_t>()},
        {"retained_bytes", usage[0]["retained_bytes"].as<std::int64_t>()},
        {"audit_state_sha256", digest_hex(sha256(Json::parse(audit[0][0].as<std::string>()).dump()))},
        {"previous_receipt_sha256", previous_receipt.is_null() ? Json(nullptr) : Json(digest_hex(sha256(previous_receipt.dump())))}};
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    return plan;
}
} // namespace

void append_recovery_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT attname,format_type(atttypid,atttypmod),attnotnull FROM pg_attribute "
        "WHERE attrelid='shin_server.server_recovery'::regclass AND attnum>0 AND NOT attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, std::string, bool>;
            static const std::set<Column> expected{{"singleton", "boolean", true}, {"last_recovery", "jsonb", false}};
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<bool>());
            if (actual != expected) throw ServiceError(503, "schema-unsupported", "Recovery schema is not supported.");
        });
    checks.add(
        "SELECT "
        "EXISTS(SELECT 1 FROM pg_constraint WHERE conrelid='shin_server.server_recovery'::regclass AND contype='p' AND conkey=ARRAY[1]::smallint[]),"
        "(SELECT count(*) FROM pg_constraint WHERE conrelid='shin_server.server_recovery'::regclass AND contype='c')=2,"
        "(SELECT count(*) FROM shin_server.server_recovery WHERE singleton)=1",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) throw ServiceError(503, "schema-unsupported", "Recovery schema constraints are not supported.");
            for (const auto& value : rows[0]) if (!value.as<bool>())
                throw ServiceError(503, "schema-unsupported", "Recovery schema constraints are not supported.");
        });
}

Json recovery_operation(pqxx::transaction_base& tx, const std::string& expected, const Json& input, bool apply,
                        IdentityMaintenance mode) {
    const bool retire = mode == IdentityMaintenance::observer_retirement;
    if (apply) {
        if (retire) {
            require_fields(input, {"plan", "discard_confirmed"});
            if (input.at("discard_confirmed") != Json(true))
                throw ServiceError(400, "invalid-recovery-request", "Retired identity metadata discard must be explicitly confirmed.");
        } else require_fields(input, {"plan"});
        require_fields(input.at("plan"), {"schema_version", "request", "new_server_id", "stream_count", "active_stream_count",
            "observer_state_sha256", "tombstone_count", "tombstone_state_sha256", "retained_submissions", "retained_bytes", "audit_state_sha256", "previous_receipt_sha256", "plan_id"});
    }
    const auto& request = apply ? input.at("plan").at("request") : input;
    validate_request(request, expected, mode);
    const auto previous = stored_recovery(tx);
    if (!apply) return Json{{"plan", plan_for(tx, request, previous, mode)}, {"read_only", true}, {"restored_backup_verified", false}};
    const auto current = read_server_identity(tx);
    if (!previous.is_null() && previous.at("plan").dump() == input.at("plan").dump() && current == successor_id(request, mode))
        return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
    const auto plan = plan_for(tx, request, previous, mode);
    if (plan.dump() != input.at("plan").dump()) throw ServiceError(409, "recovery-plan-stale", "Recovery plan no longer matches its observed metadata.");
    if (!previous.is_null() && previous.at("receipt").at("operation_id") == request.at("operation_id"))
        throw ServiceError(409, "recovery-operation-conflict", "Recovery operation identity was already consumed.");
    if (retire && plan.at("tombstone_count") == 0)
        throw ServiceError(409, "observer-retirement-empty", "No retired Observer identities require namespace retirement.");
    const auto revoked = tx.exec("UPDATE shin_server.observer_streams SET revoked=TRUE WHERE NOT revoked");
    if (revoked.affected_rows() != plan.at("active_stream_count").get<pqxx::result::size_type>())
        throw ServiceError(503, "recovery-state-inconsistent", "Observer registry changed during recovery.");
    if (retire) {
        // The exclusive lifecycle fence is held until both deletion and the new
        // deployment pin commit. No old authenticated request can join this namespace.
        const auto removed = tx.exec("DELETE FROM shin_server.observer_tombstones");
        const auto count = plan.at("tombstone_count").get<pqxx::result::size_type>();
        const auto usage = tx.exec("UPDATE shin_server.observer_usage SET tombstones=0 WHERE singleton AND tombstones=$1",
            pqxx::params{tx, count});
        if (removed.affected_rows() != count || usage.affected_rows() != 1)
            throw ServiceError(503, "recovery-state-inconsistent", "Retired identity accounting changed during maintenance.");
    }
    tx.exec("UPDATE shin_server.server_identity SET server_id=$1 WHERE singleton", pqxx::params{tx, successor_id(request, mode)});
    const auto time = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')")[0][0].as<std::string>();
    Json receipt{{"schema_version", retire ? "shin.observer-retirement-receipt.v1" : "shin.server-recovery-receipt.v2"},
        {"operation_id", request.at("operation_id")},
        {"previous_server_id", request.at("expected_server_id")}, {"server_id", plan.at("new_server_id")},
        {"reason_code", request.at("reason_code")}, {"plan_id", plan.at("plan_id")},
        {"revoked_streams", plan.at("active_stream_count")}, {"retained_streams", plan.at("stream_count")},
        {"retained_tombstones", retire ? Json(0) : plan.at("tombstone_count")},
        {"previous_receipt_sha256", plan.at("previous_receipt_sha256")}, {"recorded_at", time},
        {"service_fenced_declared", true}, {"restored_backup_verified", false}, {"history_complete", false},
        {"local_state_verified", false}, {"source_verified", false}, {"shared_publication_mutated", false}};
    if (retire) {
        receipt["retired_tombstones"] = plan.at("tombstone_count");
        receipt["retired_tombstone_state_sha256"] = plan.at("tombstone_state_sha256");
        receipt["identity_namespace_retired"] = true;
        receipt["identity_scope"] = "server-id-and-stream-id";
        receipt["discard_confirmed"] = true;
        receipt["audit_preserved"] = true;
    }
    const auto saved = tx.exec("UPDATE shin_server.server_recovery SET last_recovery=$1::jsonb WHERE singleton",
                              pqxx::params{tx, Json{{"plan", plan}, {"receipt", receipt}}.dump()});
    if (saved.affected_rows() != 1) throw ServiceError(503, "recovery-state-unavailable", "Recovery state is unavailable.");
    return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
}

Json Store::recovery_status(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id)) throw ServiceError(403, "forbidden", "Administrator access is required.");
    return run_transaction([](pqxx::transaction_base& tx) {
        const auto stored = stored_recovery(tx);
        return Json{{"schema_version", "shin.server-recovery-status.v1"},
            {"last_receipt", stored.is_null() ? Json(nullptr) : stored.at("receipt")}, {"read_only", true}, {"history_complete", false}};
    });
}
} // namespace shin::server
