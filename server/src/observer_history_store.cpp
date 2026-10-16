#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/identity.h"
#include "shin/observer_protocol.h"
#include "observer_store_internal.h"
#include <algorithm>

namespace shin::server {
namespace {
using namespace observer_store_detail;

Json plan_for(pqxx::transaction_base& tx, const pqxx::row_ref& stream) {
    const auto floor = stream["retired_sequence"].as<std::int64_t>();
    const auto confirmed = stream["confirmed_sequence"].as<std::int64_t>();
    const auto through = std::max(floor, confirmed - 1);
    const auto rows = tx.exec("SELECT count(*),COALESCE(sum(retained_bytes),0) FROM shin_server.observer_submissions "
        "WHERE stream_id=$1 AND sequence>$2 AND sequence<=$3", pqxx::params{tx, stream["stream_id"].as<std::string>(), floor, through});
    const auto count = rows[0][0].as<std::int64_t>(), bytes = rows[0][1].as<std::int64_t>();
    if (count != through - floor || count > 1023 || bytes > stream["retained_bytes"].as<std::int64_t>())
        throw ServiceError(503, "observer-history-inconsistent", "Retained history does not match its cursor.");
    Json result{{"schema_version", "shin.observer-retention-plan.v1"}, {"server_id", read_server_identity(tx)},
        {"stream_id", stream["stream_id"].as<std::string>()}, {"accepted_sequence", stream["accepted_sequence"].as<std::int64_t>()},
        {"confirmed_sequence", confirmed}, {"retired_sequence", floor}, {"through_sequence", through},
        {"delete_submissions", count}, {"delete_bytes", bytes}, {"source_verified", false},
        {"shared_publication_mutated", false}};
    result["plan_id"] = digest_hex(sha256(result.dump()));
    return result;
}

std::string reconciliation_state(pqxx::transaction_base& tx, const pqxx::row_ref& row, const Json& input, const std::string& identity) {
    if (input.at("server_id") != identity) return "server-identity-mismatch";
    if (row["revoked"].as<bool>()) return "revoked";
    const auto acknowledged = input.at("acknowledged_sequence").get<std::int64_t>();
    const auto issued = input.at("accepted_sequence").get<std::int64_t>();
    const auto accepted = row["accepted_sequence"].as<std::int64_t>();
    const auto retired = row["retired_sequence"].as<std::int64_t>();
    if (acknowledged > accepted) return "server-behind";
    if (retired != 0 && acknowledged <= retired) return "history-retired";
    if (acknowledged != 0) {
        const auto receipt = tx.exec("SELECT receipt::text FROM shin_server.observer_submissions WHERE stream_id=$1 AND sequence=$2",
            pqxx::params{tx, row["stream_id"].as<std::string>(), acknowledged});
        if (receipt.empty()) return "history-gap";
        if (Json::parse(receipt[0][0].as<std::string>()) != input.at("last_receipt")) return "receipt-conflict";
    }
    if (accepted > issued) return "server-ahead";
    if (input.at("next_submission").is_null()) return "caught-up";
    const auto& next = input.at("next_submission");
    for (const auto* key : {"project_id", "workspace_id", "observer_id", "source_host_id", "profile"})
        if (next.at("payload").at(key) != row[key].as<std::string>()) return "submission-conflict";
    if (next.at("payload").at("workspace_binding_digest") != row["binding_digest"].as<std::string>()) return "submission-conflict";
    if (acknowledged == accepted)
        return next.at("payload").at("observer_sequence").get<std::int64_t>() > row["observed_sequence"].as<std::int64_t>()
            ? "send-next" : "submission-conflict";
    const auto retained = tx.exec("SELECT submission_hash FROM shin_server.observer_submissions WHERE stream_id=$1 AND sequence=$2",
        pqxx::params{tx, row["stream_id"].as<std::string>(), next.at("sequence").get<std::int64_t>()});
    if (retained.empty()) return "history-gap";
    return retained[0][0].as<std::string>() == digest_hex(sha256(next.dump())) ? "replay-head" : "submission-conflict";
}
} // namespace

Json Store::confirm_observer(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope) {
    authorize(principal);
    require_fields(input, {"receipt"});
    const auto& receipt = input.at("receipt");
    try { observer_protocol::verify_acknowledgement_shape(receipt); }
    catch (const observer_protocol::ValidationError&) { throw ServiceError(400, "invalid-confirmation", "Receipt confirmation is invalid."); }
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::exclusive);
        const auto& row = rows[0];
        if (row["principal_id"].as<std::string>() != principal.id || row["revoked"].as<bool>())
            throw ServiceError(403, "observer-confirmation-forbidden", "Only the active enrolled producer may confirm receipt durability.");
        if (receipt.at("stream_id") != stream || receipt.at("server_id") != read_server_identity(tx))
            throw ServiceError(409, "observer-confirmation-conflict", "Receipt identity does not match the enrolled stream.");
        const auto sequence = receipt.at("sequence").get<std::int64_t>();
        if (sequence <= row["retired_sequence"].as<std::int64_t>())
            throw ServiceError(410, "observer-history-retired", "Receipt is outside retained history.");
        const auto stored = tx.exec("SELECT receipt::text FROM shin_server.observer_submissions WHERE stream_id=$1 AND sequence=$2",
            pqxx::params{tx, stream, sequence});
        if (stored.empty() || Json::parse(stored[0][0].as<std::string>()) != receipt)
            throw ServiceError(409, "observer-confirmation-conflict", "Receipt does not match retained Server evidence.");
        const auto confirmed = std::max(sequence, row["confirmed_sequence"].as<std::int64_t>());
        tx.exec("UPDATE shin_server.observer_streams SET confirmed_sequence=$2 WHERE stream_id=$1", pqxx::params{tx, stream, confirmed});
        return Json{{"schema_version", "shin.observer-confirmation.v1"}, {"server_id", receipt.at("server_id")},
            {"stream_id", stream}, {"confirmed_sequence", confirmed}, {"retired_sequence", row["retired_sequence"].as<std::int64_t>()},
            {"producer_durability_verified", false}, {"shared_publication_mutated", false}};
    });
}

Json Store::observer_retention(const Principal& principal, const std::string& stream, const std::optional<ObserverScope>& scope) {
    authorize(principal);
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::shared);
        return plan_for(tx, rows[0]);
    });
}

Json Store::apply_observer_retention(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope) {
    authorize(principal, !scope);
    require_fields(input, {"plan"});
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::exclusive);
        const auto& row = rows[0];
        if (!row["last_retention"].is_null()) {
            const auto previous = Json::parse(row["last_retention"].as<std::string>());
            if (previous.at("plan") == input.at("plan") && previous.at("plan").at("server_id") == read_server_identity(tx))
                return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
        }
        const auto plan = plan_for(tx, row);
        if (plan != input.at("plan")) throw ServiceError(409, "observer-retention-stale", "Retention plan no longer matches the stream.");
        const auto count = plan.at("delete_submissions").get<std::int64_t>();
        const auto bytes = plan.at("delete_bytes").get<std::int64_t>();
        if (count == 0) throw ServiceError(409, "observer-retention-empty", "No confirmed prefix is eligible for retirement.");
        const auto reserved = tx.exec("UPDATE shin_server.observer_usage SET submissions=submissions-$1,retained_bytes=retained_bytes-$2 "
            "WHERE singleton AND submissions>=$1 AND retained_bytes>=$2 RETURNING singleton", pqxx::params{tx, count, bytes});
        if (reserved.empty()) throw ServiceError(503, "observer-history-inconsistent", "Retained quota accounting is inconsistent.");
        const auto removed = tx.exec("DELETE FROM shin_server.observer_submissions WHERE stream_id=$1 AND sequence>$2 AND sequence<=$3",
            pqxx::params{tx, stream, plan.at("retired_sequence").get<std::int64_t>(), plan.at("through_sequence").get<std::int64_t>()});
        if (removed.affected_rows() != static_cast<pqxx::result::size_type>(count))
            throw ServiceError(503, "observer-history-inconsistent", "Retained prefix changed unexpectedly.");
        Json receipt{{"schema_version", "shin.observer-retention-receipt.v1"}, {"server_id", plan.at("server_id")},
            {"stream_id", stream}, {"plan_id", plan.at("plan_id")}, {"previous_retired_sequence", plan.at("retired_sequence")},
            {"retired_sequence", plan.at("through_sequence")}, {"deleted_submissions", count}, {"deleted_bytes", bytes},
            {"confirmed_sequence", plan.at("confirmed_sequence")}, {"accepted_sequence", plan.at("accepted_sequence")},
            {"principal_id", principal.id}, {"source_verified", false}, {"shared_publication_mutated", false}};
        const Json retained{{"plan", plan}, {"receipt", receipt}};
        tx.exec("UPDATE shin_server.observer_streams SET retired_sequence=$2,retained_bytes=retained_bytes-$3,last_retention=$4::jsonb WHERE stream_id=$1",
            pqxx::params{tx, stream, plan.at("through_sequence").get<std::int64_t>(), bytes, retained.dump()});
        audit(tx, principal, row, "observer.history.pruned", plan.at("through_sequence").get<std::int64_t>());
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}

Json Store::reconcile_observer(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope) {
    authorize(principal);
    try { observer_protocol::verify_reconciliation_request(input); }
    catch (const observer_protocol::ValidationError&) { throw ServiceError(400, "invalid-reconciliation", "Reconciliation input is invalid."); }
    if (input.at("stream_id") != stream) throw ServiceError(400, "invalid-reconciliation", "Reconciliation stream does not match its route.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::shared);
        const auto& row = rows[0];
        const auto identity = read_server_identity(tx);
        const auto state = reconciliation_state(tx, row, input, identity);
        const auto resume = state == "caught-up" || state == "send-next" || state == "replay-head";
        const auto confirmed = row["confirmed_sequence"].as<std::int64_t>();
        return Json{{"schema_version", "shin.observer-reconcile-result.v1"}, {"server_id", identity}, {"stream_id", stream},
            {"state", state}, {"local_acknowledged_sequence", input.at("acknowledged_sequence")},
            {"local_accepted_sequence", input.at("accepted_sequence")}, {"server_accepted_sequence", row["accepted_sequence"].as<std::int64_t>()},
            {"confirmed_sequence", confirmed}, {"retired_sequence", row["retired_sequence"].as<std::int64_t>()},
            {"retained_submissions", row["accepted_sequence"].as<std::int64_t>() - row["retired_sequence"].as<std::int64_t>()},
            {"retained_bytes", row["retained_bytes"].as<std::int64_t>()},
            {"confirmation_required", resume && input.at("acknowledged_sequence").get<std::int64_t>() > confirmed},
            {"next_action", resume ? state == "caught-up" ? "none" : state : "operator-review"},
            {"resume_allowed", resume}, {"observer_state_mutated", false}, {"shared_publication_mutated", false},
            {"source_verified", false}, {"local_state_verified", false}, {"verification_level", "service-observed"}};
    });
}
} // namespace shin::server
