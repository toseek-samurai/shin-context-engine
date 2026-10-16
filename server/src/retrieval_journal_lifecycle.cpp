#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/retrieval_journal.h"

#include <pqxx/pqxx>

namespace shin::server {
namespace {
void admin(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id))
        throw ServiceError(403, "forbidden", "Administrator access is required.");
}

pqxx::result lifecycle(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT l.*,i.server_id FROM shin_server.retrieval_journal_lifecycle l "
        "CROSS JOIN shin_server.server_identity i WHERE l.singleton AND i.singleton");
    if (rows.size() != 1)
        throw ServiceError(503, "journal-progress-inconsistent", "Journal lifecycle identity is unavailable.");
    return rows;
}

Json last_receipt(const pqxx::row_ref& state) {
    return state["last_reclamation"].is_null() ? Json(nullptr) :
        Json::parse(state["last_reclamation"].as<std::string>()).at("receipt");
}

Json make_plan(pqxx::transaction_base& tx, const pqxx::row_ref& state, std::int64_t through,
               bool revalidating = false) {
    const auto floor = state["reclaimed_through"].as<std::int64_t>();
    if (through <= floor || through > state["allocated_through"].as<std::int64_t>())
        throw ServiceError(409, "journal-reclamation-range", "Choose an allocated epoch prefix above the reclamation fence.");
    if (through - floor > static_cast<std::int64_t>(journal::maximum_reclaim_producers))
        throw ServiceError(409, "journal-reclamation-too-large", "Inspect at most 32 producer epochs at once.");
    const auto rows = tx.exec("SELECT p.*,EXISTS(SELECT 1 FROM shin_server.retrieval_journal_attempts j "
        "WHERE j.producer_epoch=p.producer_epoch) AS has_facts FROM shin_server.retrieval_journal_producers p "
        "WHERE producer_epoch>$1 AND producer_epoch<=$2 ORDER BY producer_epoch",
        pqxx::params{tx, floor, through});
    if (static_cast<std::int64_t>(rows.size()) != through - floor)
        throw ServiceError(503, "journal-progress-inconsistent", "The allocated producer prefix is incomplete.");
    Json producers = Json::array();
    for (const auto& row : rows) {
        if (row["observed_through"].as<std::int64_t>() != row["retired_through"].as<std::int64_t>() ||
            row["captured_count"].as<std::int64_t>() != row["retired_count"].as<std::int64_t>() ||
            row["missing_count"].as<std::int64_t>() != row["retired_missing_count"].as<std::int64_t>() ||
            row["has_facts"].as<bool>())
            throw ServiceError(409, revalidating ? "journal-reclamation-stale" : "journal-producers-not-retired",
                               "Retire every observed sequence in the selected producer prefix first.");
        Json item;
        for (const auto* key : {"instance_id", "source_host_id", "server_version", "core_version"})
            item[key] = row[key].as<std::string>();
        for (const auto* key : {"producer_epoch", "observed_through", "captured_count", "missing_count"})
            item[key] = row[key].as<std::int64_t>();
        producers.push_back(std::move(item));
    }
    const auto totals = tx.exec("SELECT COALESCE(sum(captured_count),0)::bigint AS captured,"
        "COALESCE(sum(missing_count),0)::bigint AS missing FROM shin_server.retrieval_journal_producers "
        "WHERE producer_epoch>$1 AND producer_epoch<=$2", pqxx::params{tx, floor, through});
    const auto previous = last_receipt(state);
    Json plan{{"schema_version", "shin.retrieval-journal-reclamation-plan.v1"},
        {"server_id", state["server_id"].as<std::string>()}, {"scope", "all-producers-in-epoch-prefix"},
        {"reclaimed_through", floor}, {"through", through}, {"producer_count", rows.size()},
        {"captured_count", totals[0]["captured"].as<std::int64_t>()},
        {"missing_count", totals[0]["missing"].as<std::int64_t>()},
        {"prefix_sha256", digest_hex(sha256(producers.dump()))}, {"producers", std::move(producers)},
        {"previous_receipt_sha256", previous.is_null() ? Json(nullptr) : Json(digest_hex(sha256(previous.dump())))}};
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    return plan;
}

std::int64_t validate_plan(const Json& input) {
    require_fields(input, {"plan", "discard_future_confirmed"});
    if (input.at("discard_future_confirmed") != Json(true))
        throw ServiceError(400, "invalid-request", "Explicit acknowledgement of future capture refusal is required.");
    const auto& plan = input.at("plan");
    require_fields(plan, {"schema_version", "server_id", "scope", "reclaimed_through", "through", "producer_count",
        "captured_count", "missing_count", "producers", "prefix_sha256", "previous_receipt_sha256", "plan_id"});
    const auto& through = plan.at("through");
    if (!through.is_number_integer() || through <= 0 || through > INT64_MAX || plan.dump().size() > 32768)
        throw ServiceError(400, "invalid-request", "Journal reclamation plan bounds are invalid.");
    return through.get<std::int64_t>();
}
} // namespace

Json Store::register_journal_producer(const Principal& principal, const Json& input) {
    admin(principal);
    const auto provenance = journal::normalize_producer(input);
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto states = lifecycle(tx);
        const auto& state = states[0];
        const auto instance = provenance.at("instance_id").get<std::string>();
        const auto existing = tx.exec("SELECT * FROM shin_server.retrieval_journal_producers WHERE instance_id=$1",
                                      pqxx::params{tx, instance});
        std::int64_t epoch = 0;
        if (!existing.empty()) {
            for (const auto* key : {"source_host_id", "server_version", "core_version"})
                if (provenance.at(key) != existing[0][key].as<std::string>())
                    throw ServiceError(409, "journal-producer-conflict", "A producer cannot change its provenance.");
            epoch = existing[0]["producer_epoch"].as<std::int64_t>();
            if (epoch <= state["reclaimed_through"].as<std::int64_t>())
                throw ServiceError(409, "journal-producer-reclaimed", "The producer epoch was reclaimed.");
            if (epoch > state["allocated_through"].as<std::int64_t>())
                throw ServiceError(503, "journal-progress-inconsistent", "The registered producer exceeds allocated progress.");
        } else {
            if (tx.exec("SELECT count(*) FROM shin_server.retrieval_journal_producers")[0][0].as<std::size_t>() >= journal::maximum_producers)
                throw ServiceError(507, "journal-capacity", "The bounded journal producer capacity is exhausted.");
            const auto allocated = state["allocated_through"].as<std::int64_t>();
            if (allocated == INT64_MAX)
                throw ServiceError(507, "journal-epoch-exhausted", "The journal epoch space is exhausted.");
            epoch = allocated + 1;
            tx.exec("INSERT INTO shin_server.retrieval_journal_producers(instance_id,source_host_id,server_version,core_version,producer_epoch) "
                "VALUES($1,$2,$3,$4,$5)", pqxx::params{tx, instance, provenance.at("source_host_id").get<std::string>(),
                    provenance.at("server_version").get<std::string>(), provenance.at("core_version").get<std::string>(), epoch});
            tx.exec("UPDATE shin_server.retrieval_journal_lifecycle SET allocated_through=$1 WHERE singleton", pqxx::params{tx, epoch});
        }
        return Json{{"schema_version", "shin.retrieval-journal-producer.v1"}, {"instance_id", instance},
                    {"producer_epoch", epoch}, {"replayed", !existing.empty()}};
    });
}

Json Store::plan_journal_reclamation(const Principal& principal, std::uint64_t through) {
    admin(principal);
    if (through == 0 || through > INT64_MAX)
        throw ServiceError(400, "invalid-query", "Journal epoch range is invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto states = lifecycle(tx);
        return make_plan(tx, states[0], static_cast<std::int64_t>(through));
    });
}

Json Store::apply_journal_reclamation(const Principal& principal, const Json& input) {
    admin(principal);
    const auto through = validate_plan(input);
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto states = lifecycle(tx);
        const auto& state = states[0];
        const auto& requested = input.at("plan");
        if (requested.at("server_id") != state["server_id"].as<std::string>())
            throw ServiceError(409, "journal-reclamation-stale", "Journal deployment identity changed.");
        if (!state["last_reclamation"].is_null()) {
            const auto previous = Json::parse(state["last_reclamation"].as<std::string>());
            if (previous.at("plan").dump() == requested.dump())
                return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
        }
        if (requested.at("reclaimed_through") != state["reclaimed_through"].as<std::int64_t>())
            throw ServiceError(409, "journal-reclamation-stale", "Journal reclamation fence changed.");
        const auto plan = make_plan(tx, state, through, true);
        if (plan.dump() != requested.dump())
            throw ServiceError(409, "journal-reclamation-stale", "Producer progress changed; inspect a fresh plan.");
        const auto removed = tx.exec("DELETE FROM shin_server.retrieval_journal_producers WHERE producer_epoch>$1 AND producer_epoch<=$2",
            pqxx::params{tx, state["reclaimed_through"].as<std::int64_t>(), through});
        if (static_cast<std::int64_t>(removed.affected_rows()) != plan.at("producer_count").get<std::int64_t>())
            throw ServiceError(503, "journal-progress-inconsistent", "The producer prefix changed.");
        const auto clock = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
        Json receipt{{"schema_version", "shin.retrieval-journal-reclamation-receipt.v1"}, {"server_id", plan.at("server_id")},
            {"plan_id", plan.at("plan_id")}, {"previous_reclaimed_through", plan.at("reclaimed_through")}, {"reclaimed_through", through},
            {"producer_count", plan.at("producer_count")}, {"captured_count", plan.at("captured_count")},
            {"missing_count", plan.at("missing_count")}, {"previous_receipt_sha256", plan.at("previous_receipt_sha256")},
            {"principal_id", principal.id}, {"recorded_at", clock[0][0].as<std::string>()}, {"discard_future_confirmed", true},
            {"external_archive_verified", false}, {"source_mutated", false}, {"publication_mutated", false}};
        const Json saved{{"plan", plan}, {"receipt", receipt}};
        tx.exec("UPDATE shin_server.retrieval_journal_lifecycle SET reclaimed_through=$1,"
            "reclaimed_producer_count=reclaimed_producer_count+$2,reclaimed_captured_count=reclaimed_captured_count+$3,"
            "reclaimed_missing_count=reclaimed_missing_count+$4,last_reclamation=$5::jsonb WHERE singleton",
            pqxx::params{tx, through, plan.at("producer_count").get<std::int64_t>(), plan.at("captured_count").get<std::int64_t>(),
                         plan.at("missing_count").get<std::int64_t>(), saved.dump()});
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}
} // namespace shin::server
