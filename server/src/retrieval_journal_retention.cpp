#include "shin/server/store.h"
#include "shin/server/config.h"

#include <pqxx/pqxx>
#include <map>

namespace shin::server {
namespace {
void admin(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id))
        throw ServiceError(403, "forbidden", "Administrator access is required.");
}

bool instance_id(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

pqxx::result producer(pqxx::transaction_base& tx, const std::string& instance) {
    const auto rows = tx.exec("SELECT p.*,i.server_id FROM shin_server.retrieval_journal_producers p "
        "CROSS JOIN shin_server.server_identity i WHERE p.instance_id=$1 AND i.singleton", pqxx::params{tx, instance});
    if (rows.empty()) throw ServiceError(404, "journal-producer-not-found", "Journal producer is unavailable.");
    return rows;
}

Json previous_receipt(const pqxx::row_ref& row) {
    return row["last_retention"].is_null() ? Json(nullptr) :
        Json::parse(row["last_retention"].as<std::string>()).at("receipt");
}

Json make_plan(pqxx::transaction_base& tx, const pqxx::row_ref& state, std::int64_t through, bool revalidating = false) {
    const auto floor = state["retired_through"].as<std::int64_t>();
    if (through <= floor || through > state["observed_through"].as<std::int64_t>())
        throw ServiceError(409, "journal-retention-range", "Choose a nonempty observed prefix above the retirement fence.");
    const auto rows = tx.exec("SELECT id,sequence,record_hash,project_id FROM shin_server.retrieval_journal_attempts "
        "WHERE instance_id=$1 AND sequence>$2 AND sequence<=$3 ORDER BY sequence LIMIT 257",
        pqxx::params{tx, state["instance_id"].as<std::string>(), floor, through});
    if (rows.size() > 256)
        throw ServiceError(409, revalidating ? "journal-retention-stale" : "journal-retention-too-large",
                           "Inspect a prefix containing at most 256 retained facts.");
    Json prefix = Json::array();
    std::map<std::string, std::int64_t> counts;
    for (const auto& row : rows) {
        const auto project = row["project_id"].as<std::string>();
        prefix.push_back(Json::array({row["id"].as<std::int64_t>(), row["sequence"].as<std::int64_t>(),
                                     row["record_hash"].as<std::string>(), project}));
        ++counts[project];
    }
    Json projects = Json::array();
    for (const auto& [id, count] : counts) projects.push_back({{"project_id", id}, {"delete_count", count}});
    const auto missing = through - floor - static_cast<std::int64_t>(rows.size());
    if (missing < 0) throw ServiceError(503, "journal-progress-inconsistent", "Journal prefix is inconsistent.");
    const auto previous = previous_receipt(state);
    Json plan{{"schema_version", "shin.retrieval-journal-retention-plan.v1"},
        {"server_id", state["server_id"].as<std::string>()}, {"instance_id", state["instance_id"].as<std::string>()},
        {"producer_epoch", state["producer_epoch"].as<std::int64_t>()},
        {"scope", "all-projects-of-producer"}, {"retired_through", floor}, {"through", through},
        {"delete_count", rows.size()}, {"retire_missing_count", missing}, {"affected_projects", std::move(projects)},
        {"prefix_sha256", digest_hex(sha256(prefix.dump()))},
        {"previous_receipt_sha256", previous.is_null() ? Json(nullptr) : Json(digest_hex(sha256(previous.dump())))}};
    plan["plan_id"] = digest_hex(sha256(plan.dump()));
    return plan;
}

std::int64_t validate_plan(const Json& input) {
    require_fields(input, {"plan", "discard_confirmed"});
    if (input.at("discard_confirmed") != Json(true))
        throw ServiceError(400, "invalid-request", "Explicit journal discard confirmation is required.");
    const auto& plan = input.at("plan");
    require_fields(plan, {"schema_version", "server_id", "instance_id", "scope", "retired_through", "through",
        "producer_epoch", "delete_count", "retire_missing_count", "affected_projects", "prefix_sha256", "previous_receipt_sha256", "plan_id"});
    const auto& through = plan.at("through");
    if (!through.is_number_integer() || through <= 0 || through > INT64_MAX || plan.dump().size() > 32768)
        throw ServiceError(400, "invalid-request", "Journal retention plan bounds are invalid.");
    return through.get<std::int64_t>();
}
} // namespace

Json Store::retrieval_journal_producers(const Principal& principal, const std::string& after, std::size_t limit) {
    admin(principal);
    if ((!after.empty() && !instance_id(after)) || limit == 0 || limit > 32)
        throw ServiceError(400, "invalid-query", "Journal producer page parameters are invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto rows = tx.exec("SELECT * FROM shin_server.retrieval_journal_producers WHERE instance_id>$1 "
            "ORDER BY instance_id LIMIT $2", pqxx::params{tx, after, static_cast<std::int64_t>(limit + 1)});
        Json items = Json::array();
        for (const auto& row : rows) {
            if (items.size() == limit) break;
            Json item;
            for (const auto* key : {"instance_id", "source_host_id", "server_version", "core_version"})
                item[key] = row[key].as<std::string>();
            for (const auto* key : {"producer_epoch", "observed_through", "captured_count", "missing_count", "retired_through", "retired_count", "retired_missing_count"})
                item[key] = row[key].as<std::int64_t>();
            item["retained_count"] = row["captured_count"].as<std::int64_t>() - row["retired_count"].as<std::int64_t>();
            item["last_retention_receipt"] = previous_receipt(row);
            items.push_back(std::move(item));
        }
        return Json{{"schema_version", "shin.retrieval-journal-producers.v1"}, {"producers", items},
            {"next_after", static_cast<std::size_t>(rows.size()) > limit ? items.back().at("instance_id") : Json(nullptr)}, {"read_only", true}};
    });
}

Json Store::plan_journal_retention(const Principal& principal, const std::string& instance, std::uint64_t through) {
    admin(principal);
    if (!instance_id(instance) || through == 0 || through > INT64_MAX)
        throw ServiceError(400, "invalid-query", "Journal retention parameters are invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto rows = producer(tx, instance);
        return make_plan(tx, rows[0], static_cast<std::int64_t>(through));
    });
}

Json Store::apply_journal_retention(const Principal& principal, const std::string& instance, const Json& input) {
    admin(principal);
    if (!instance_id(instance)) throw ServiceError(400, "invalid-query", "Journal producer identity is invalid.");
    const auto through = validate_plan(input);
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto rows = producer(tx, instance);
        const auto& state = rows[0];
        const auto& requested = input.at("plan");
        if (requested.at("server_id") != state["server_id"].as<std::string>() || requested.at("instance_id") != instance ||
            requested.at("producer_epoch") != state["producer_epoch"].as<std::int64_t>())
            throw ServiceError(409, "journal-retention-stale", "Journal retention identity changed.");
        if (!state["last_retention"].is_null()) {
            const auto previous = Json::parse(state["last_retention"].as<std::string>());
            if (previous.at("plan").dump() == requested.dump())
                return Json{{"receipt", previous.at("receipt")}, {"replayed", true}};
        }
        if (requested.at("retired_through") != state["retired_through"].as<std::int64_t>())
            throw ServiceError(409, "journal-retention-stale", "Journal retirement fence changed.");
        const auto plan = make_plan(tx, state, through, true);
        if (plan.dump() != requested.dump())
            throw ServiceError(409, "journal-retention-stale", "Journal prefix changed; inspect a fresh plan.");
        const auto removed = tx.exec("DELETE FROM shin_server.retrieval_journal_attempts WHERE instance_id=$1 AND sequence>$2 AND sequence<=$3",
            pqxx::params{tx, instance, state["retired_through"].as<std::int64_t>(), through});
        if (static_cast<std::int64_t>(removed.affected_rows()) != plan.at("delete_count").get<std::int64_t>())
            throw ServiceError(503, "journal-progress-inconsistent", "Journal retirement count changed.");
        const auto clock = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
        Json receipt{{"schema_version", "shin.retrieval-journal-retention-receipt.v1"}, {"server_id", plan.at("server_id")},
            {"instance_id", instance}, {"plan_id", plan.at("plan_id")}, {"previous_retired_through", plan.at("retired_through")},
            {"retired_through", through}, {"deleted_count", plan.at("delete_count")}, {"retired_missing_count", plan.at("retire_missing_count")},
            {"previous_receipt_sha256", plan.at("previous_receipt_sha256")}, {"principal_id", principal.id},
            {"recorded_at", clock[0][0].as<std::string>()}, {"discard_confirmed", true}, {"external_archive_verified", false},
            {"source_mutated", false}, {"publication_mutated", false}};
        const Json saved{{"plan", plan}, {"receipt", receipt}};
        tx.exec("UPDATE shin_server.retrieval_journal_producers SET retired_through=$2,retired_count=retired_count+$3,"
            "retired_missing_count=retired_missing_count+$4,last_retention=$5::jsonb,updated_at=clock_timestamp() WHERE instance_id=$1",
            pqxx::params{tx, instance, through, plan.at("delete_count").get<std::int64_t>(),
                         plan.at("retire_missing_count").get<std::int64_t>(), saved.dump()});
        return Json{{"receipt", std::move(receipt)}, {"replayed", false}};
    });
}
} // namespace shin::server
