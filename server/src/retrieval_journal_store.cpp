#include "shin/server/store.h"
#include "shin/server/config.h"
#include "shin/server/retrieval_journal.h"
#include "shin/server/identity_directory.h"

#include <pqxx/pqxx>
#include <algorithm>
#include <set>

namespace shin::server {
namespace {
void admin(const Principal& principal) {
    if (!principal.admin || !valid_identifier(principal.id))
        throw ServiceError(403, "forbidden", "Administrator access is required.");
}

constexpr auto columns =
    "instance_id,sequence,record_hash,project_id,operation,selector_kind,selector_id,observed_at_unix_ms,status,reason_code,"
    "last_phase,elapsed_ns,result_bytes,validation_ns,selection_ns,admission_ns,execution_ns,finalization_ns,route_id,"
    "workspace_id,source_host_id,snapshot_id,preparation_id,revision,profile_id,profile_fingerprint,route_fingerprint,"
    "retrieval_mode,returned_records,candidate_rows,covered_groups,total_groups,pack_complete,pack_coverage_complete,"
    "required_requirements,satisfied_required_requirements,query_capture,request_fingerprint,request,producer_epoch,"
    "scope_principal_id,scope_tenant_id,scope_workspace_id";

void validate_attribution(pqxx::transaction_base& tx, const Json& pending) {
    if (std::none_of(pending.begin(), pending.end(), [](const Json& row) { return row.contains("scope_principal_id"); })) return;
    // Validate historical ownership, not current enabled state: delayed capture must survive revocation.
    const auto prior = tx.exec("SELECT current_setting('shin.identity_ops',true)")[0][0].as<std::string>("");
    tx.exec("SELECT set_config('shin.identity_ops','1',true)");
    const auto invalid = tx.exec("SELECT 1 FROM jsonb_populate_recordset(NULL::shin_server.retrieval_journal_attempts,$1::jsonb) f "
        "WHERE f.scope_principal_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM shin_server.identity_workspace_owners w "
        "JOIN shin_server.identity_accounts a ON a.id=w.owner_id JOIN shin_server.identity_tenants t ON t.id=w.tenant_id "
        "WHERE w.project_id=f.project_id AND w.workspace_id=f.scope_workspace_id AND w.tenant_id=f.scope_tenant_id "
        "AND a.principal_id=f.scope_principal_id AND t.owner_id=w.owner_id) LIMIT 1", pqxx::params{tx, pending.dump()});
    tx.exec("SELECT set_config('shin.identity_ops',$1,true)", pqxx::params{tx, prior});
    if (!invalid.empty()) throw ServiceError(409, "journal-attribution-invalid", "Journal attribution does not match its ownership binding.");
}

Json coverage(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT p.producers,p.captured,l.allocated_through,l.reclaimed_through,l.reclaimed_producer_count,"
        "(p.lifetime+l.reclaimed_captured_count)::bigint AS lifetime,(p.retired+l.reclaimed_captured_count)::bigint AS retired,"
        "(p.retired_missing+l.reclaimed_missing_count)::bigint AS retired_missing,(p.missing+l.reclaimed_missing_count)::bigint AS missing "
        "FROM (SELECT count(*) AS producers,COALESCE(sum(captured_count-retired_count),0)::bigint AS captured,"
        "COALESCE(sum(captured_count),0) AS lifetime,COALESCE(sum(retired_count),0) AS retired,"
        "COALESCE(sum(retired_missing_count),0) AS retired_missing,"
        "COALESCE(sum(missing_count),0) AS missing FROM shin_server.retrieval_journal_producers) p "
        "CROSS JOIN shin_server.retrieval_journal_lifecycle l WHERE l.singleton");
    if (rows.size() != 1)
        throw ServiceError(503, "journal-progress-inconsistent", "Journal lifecycle identity is unavailable.");
    return {{"population", "persisted-journal-facts-only"}, {"all_traffic_complete", false},
        {"producer_count", rows[0]["producers"].as<std::int64_t>()},
        {"captured_count", rows[0]["captured"].as<std::int64_t>()},
        {"lifetime_captured_count", rows[0]["lifetime"].as<std::int64_t>()},
        {"retired_fact_count", rows[0]["retired"].as<std::int64_t>()},
        {"retired_missing_sequences_global", rows[0]["retired_missing"].as<std::int64_t>()},
        {"known_missing_sequences_global", rows[0]["missing"].as<std::int64_t>()},
        {"allocated_producer_epochs", rows[0]["allocated_through"].as<std::int64_t>()},
        {"reclaimed_through_epoch", rows[0]["reclaimed_through"].as<std::int64_t>()},
        {"reclaimed_producer_count", rows[0]["reclaimed_producer_count"].as<std::int64_t>()},
        {"unknown_after_last_capture", true}, {"feedback_available", false}};
}

Json parse_row(const pqxx::row_ref& row) { return Json::parse(row["value"].as<std::string>()); }
} // namespace

Json Store::capture_retrieval_journal(const Principal& principal, const Json& input) {
    admin(principal);
    const auto batch = journal::normalize_batch(input);
    return run_transaction([&](pqxx::transaction_base& tx) {
        if (std::any_of(batch.at("facts").begin(), batch.at("facts").end(),
                        [](const Json& row) { return row.contains("scope_principal_id"); }))
            tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto instance = batch.at("instance_id").get<std::string>();
        const auto host = batch.at("source_host_id").get<std::string>();
        const auto server = batch.at("server_version").get<std::string>();
        const auto core = batch.at("core_version").get<std::string>();
        const auto epoch = batch.at("producer_epoch").get<std::int64_t>();
        const auto states = tx.exec("SELECT reclaimed_through,allocated_through FROM shin_server.retrieval_journal_lifecycle WHERE singleton");
        if (states.size() != 1)
            throw ServiceError(503, "journal-progress-inconsistent", "Journal lifecycle identity is unavailable.");
        if (epoch <= states[0][0].as<std::int64_t>())
            throw ServiceError(409, "journal-producer-reclaimed", "The producer epoch was reclaimed; it cannot capture again.");
        if (epoch > states[0][1].as<std::int64_t>())
            throw ServiceError(409, "journal-producer-conflict", "The producer epoch was not allocated.");
        const auto sources = tx.exec("SELECT * FROM shin_server.retrieval_journal_producers WHERE instance_id=$1",
                               pqxx::params{tx, instance});
        if (sources.empty() || sources[0]["producer_epoch"].as<std::int64_t>() != epoch)
            throw ServiceError(409, "journal-producer-conflict", "Capture requires the exact registered producer epoch.");
        const auto& producer = sources[0];
        if (producer["source_host_id"].as<std::string>() != host || producer["server_version"].as<std::string>() != server ||
            producer["core_version"].as<std::string>() != core)
            throw ServiceError(409, "journal-producer-conflict", "A process identity cannot change its provenance.");
        const auto previous = producer["observed_through"].as<std::int64_t>();
        const auto latest = batch.at("latest_sequence").get<std::int64_t>();
        const auto floor = producer["retired_through"].as<std::int64_t>();
        Json eligible = Json::array();
        for (const auto& fact : batch.at("facts")) if (fact.at("sequence") > floor) eligible.push_back(fact);
        const auto ignored = batch.at("facts").size() - eligible.size();
        const auto encoded = eligible.dump();
        const auto observed = tx.exec("SELECT n.sequence,j.id IS NULL AS is_new,"
            "(j.id IS NOT NULL AND n.record_hash<>j.record_hash) AS conflict "
            "FROM jsonb_populate_recordset(NULL::shin_server.retrieval_journal_attempts,$1::jsonb) n "
            "LEFT JOIN shin_server.retrieval_journal_attempts j USING(instance_id,sequence)",
            pqxx::params{tx, encoded});
        std::set<std::int64_t> unseen;
        for (const auto& row : observed) {
            if (row["conflict"].as<bool>())
                throw ServiceError(409, "journal-fact-conflict", "An existing observation cannot change its content.");
            if (row["is_new"].as<bool>()) unseen.insert(row["sequence"].as<std::int64_t>());
        }
        Json pending = Json::array();
        for (const auto& row : eligible) if (unseen.contains(row.at("sequence").get<std::int64_t>())) pending.push_back(row);
        const auto count = tx.exec("SELECT count(*) FROM shin_server.retrieval_journal_attempts")[0][0].as<std::size_t>();
        if (pending.size() > journal::maximum_facts - std::min(count, journal::maximum_facts))
            throw ServiceError(507, "journal-capacity", "The bounded journal fact capacity is exhausted.");
        if (!pending.empty()) {
            validate_attribution(tx, pending);
            tx.exec(std::string("INSERT INTO shin_server.retrieval_journal_attempts(") + columns + ") SELECT " + columns +
                " FROM jsonb_populate_recordset(NULL::shin_server.retrieval_journal_attempts,$1::jsonb) ORDER BY sequence",
                pqxx::params{tx, pending.dump()});
        }
        const auto advanced = std::max(latest, previous) - previous;
        const auto recovered = static_cast<std::int64_t>(std::count_if(pending.begin(), pending.end(),
            [&](const Json& row) { return row.at("sequence") <= previous; }));
        const auto newly_missing = advanced - (static_cast<std::int64_t>(pending.size()) - recovered);
        const auto recoverable = producer["missing_count"].as<std::int64_t>() - producer["retired_missing_count"].as<std::int64_t>();
        if (newly_missing < 0 || recoverable < recovered)
            throw ServiceError(500, "journal-progress-inconsistent", "The persisted observation prefix is inconsistent.");
        if (advanced != 0 || !pending.empty()) tx.exec("UPDATE shin_server.retrieval_journal_producers SET observed_through=$2,"
            "captured_count=captured_count+$3,missing_count=missing_count+$4,updated_at=clock_timestamp() WHERE instance_id=$1",
            pqxx::params{tx, instance, std::max(latest, previous), static_cast<std::int64_t>(pending.size()), newly_missing - recovered});
        return Json{{"schema_version", "shin.retrieval-journal-capture.v1"}, {"instance_id", instance},
            {"producer_epoch", epoch}, {"inserted_count", pending.size()}, {"already_observed_count", eligible.size() - pending.size()},
            {"retired_ignored_count", ignored}, {"retired_through", floor},
            {"new_missing_sequences", newly_missing},
            {"recovered_missing_sequences", recovered}, {"observed_through", std::max(latest, previous)},
            {"replayed", advanced == 0 && pending.empty()}, {"coverage", coverage(tx)}, {"source_mutated", false},
            {"publication_mutated", false}};
    });
}

Json Store::retrieval_journal(const Principal& principal, const std::string& project, std::uint64_t after, std::size_t limit) {
    admin(principal);
    if ((!project.empty() && !valid_identifier(project)) || after > INT64_MAX || limit == 0 || limit > 32)
        throw ServiceError(400, "invalid-query", "Journal page parameters are invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto rows = tx.exec("SELECT (to_jsonb(j)-'request')::text AS value FROM shin_server.retrieval_journal_attempts j "
            "WHERE id>$1 AND ($2='' OR project_id=$2) ORDER BY id LIMIT $3",
            pqxx::params{tx, static_cast<std::int64_t>(after), project, static_cast<std::int64_t>(limit + 1)});
        Json items = Json::array();
        for (const auto& row : rows) if (items.size() < limit) items.push_back(parse_row(row));
        return Json{{"schema_version", "shin.retrieval-journal-page.v1"}, {"items", items},
            {"next_after", static_cast<std::size_t>(rows.size()) > limit ? items.back().at("id") : Json(nullptr)},
            {"coverage", coverage(tx)}, {"read_only", true}};
    });
}

Json Store::retrieval_journal_attempt(const Principal& principal, std::uint64_t id) {
    admin(principal);
    if (id == 0 || id > INT64_MAX) throw ServiceError(400, "invalid-query", "Journal ID is invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const auto rows = tx.exec("SELECT to_jsonb(j)::text AS value FROM shin_server.retrieval_journal_attempts j WHERE id=$1",
            pqxx::params{tx, static_cast<std::int64_t>(id)});
        if (rows.empty()) throw ServiceError(404, "journal-attempt-not-found", "The journal attempt is unavailable.");
        return Json{{"schema_version", "shin.retrieval-journal-detail.v1"}, {"attempt", parse_row(rows[0])}, {"read_only", true}};
    });
}

Json Store::scoped_retrieval_journal(const Principal& principal, const std::string& project,
                                    const WorkspaceReadRequest& scope, std::uint64_t after, std::size_t limit) {
    if (after > INT64_MAX || limit == 0 || limit > 32)
        throw ServiceError(400, "invalid-query", "Journal page parameters are invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
        const auto rows = tx.exec("SELECT (to_jsonb(j)-ARRAY['request','instance_id','sequence','producer_epoch','record_hash'])::text AS value "
            "FROM shin_server.retrieval_journal_attempts j WHERE scope_principal_id=$1 AND project_id=$2 "
            "AND scope_tenant_id=$3 AND scope_workspace_id=$4 AND id>$5 ORDER BY id LIMIT $6",
            pqxx::params{tx, principal.id, project, scope.tenant_id, scope.workspace_id,
                static_cast<std::int64_t>(after), static_cast<std::int64_t>(limit + 1)});
        Json items = Json::array();
        for (const auto& row : rows) if (items.size() < limit) items.push_back(parse_row(row));
        return Json{{"schema_version", "shin.scoped-retrieval-journal-page.v1"}, {"project_id", project},
            {"tenant_id", scope.tenant_id}, {"workspace_id", scope.workspace_id}, {"items", items},
            {"next_after", static_cast<std::size_t>(rows.size()) > limit ? items.back().at("id") : Json(nullptr)},
            {"history_complete", false}, {"read_only", true}};
    });
}

Json Store::scoped_retrieval_journal_attempt(const Principal& principal, const std::string& project,
                                            const WorkspaceReadRequest& scope, std::uint64_t id) {
    if (id == 0 || id > INT64_MAX) throw ServiceError(400, "invalid-query", "Journal ID is invalid.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
        const auto rows = tx.exec("SELECT (to_jsonb(j)-ARRAY['instance_id','sequence','producer_epoch','record_hash'])::text AS value "
            "FROM shin_server.retrieval_journal_attempts j WHERE scope_principal_id=$1 AND project_id=$2 "
            "AND scope_tenant_id=$3 AND scope_workspace_id=$4 AND id=$5",
            pqxx::params{tx, principal.id, project, scope.tenant_id, scope.workspace_id, static_cast<std::int64_t>(id)});
        if (rows.empty()) throw ServiceError(404, "journal-attempt-not-found", "The journal attempt is unavailable.");
        return Json{{"schema_version", "shin.scoped-retrieval-journal-detail.v1"}, {"attempt", parse_row(rows[0])}, {"read_only", true}};
    });
}

Json Store::retrieval_journal_report(const Principal& principal, const std::string& project, bool query_groups) {
    admin(principal);
    if (!valid_identifier(project)) throw ServiceError(400, "invalid-query", "An explicit project ID is required.");
    return run_transaction([&](pqxx::transaction_base& tx) {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.retrieval-journal-writers.v1')),set_config('shin.journal_ops','1',true)");
        const std::string dimensions = "j.operation,j.selector_kind,j.selector_id,j.profile_id,j.profile_fingerprint,j.route_fingerprint,"
            "j.snapshot_id,p.server_version,p.core_version";
        const auto keys = dimensions + (query_groups ? ",j.request_fingerprint" : "");
        const std::string measurements = ",count(*)::bigint AS attempts,min(j.id) AS example_attempt_id,"
            "count(*) FILTER(WHERE status=200)::bigint AS successes,count(*) FILTER(WHERE status<>200)::bigint AS errors,"
            "sum(elapsed_ns) AS elapsed_total_ns,"
            "percentile_cont(0.95) WITHIN GROUP(ORDER BY elapsed_ns) FILTER(WHERE status=200) AS success_p95_ns,"
            "percentile_cont(0.95) WITHIN GROUP(ORDER BY elapsed_ns) FILTER(WHERE status<>200) AS error_p95_ns,"
            "count(pack_complete)::bigint AS known_pack_count,count(*) FILTER(WHERE pack_complete=false)::bigint AS incomplete_pack_count,"
            "count(returned_records)::bigint AS known_result_count,count(*) FILTER(WHERE returned_records=0)::bigint AS zero_result_count,"
            "count(covered_groups)::bigint AS known_coverage_count,"
            "count(*) FILTER(WHERE request_fingerprint IS NOT NULL)::bigint AS captured_request_count,"
            "count(selection_ns)::bigint AS selection_count,count(admission_ns)::bigint AS admission_count,"
            "count(execution_ns)::bigint AS execution_count,count(finalization_ns)::bigint AS finalization_count,"
            "sum(validation_ns) AS validation_total_ns,sum(selection_ns) AS selection_total_ns,"
            "sum(admission_ns) AS admission_total_ns,sum(execution_ns) AS execution_total_ns,sum(finalization_ns) AS finalization_total_ns";
        const std::string filter = query_groups ? " AND j.request_fingerprint IS NOT NULL" : "";
        const auto rows = tx.exec("WITH grouped AS (SELECT " + keys + measurements +
            " FROM shin_server.retrieval_journal_attempts j JOIN shin_server.retrieval_journal_producers p USING(instance_id) "
            "WHERE j.project_id=$1" + filter + " GROUP BY " + keys + ") "
            "SELECT to_jsonb(g)::text AS value,count(*) OVER() AS total_groups FROM grouped g "
            "ORDER BY " + (query_groups ? std::string("attempts DESC,") : std::string()) +
            "elapsed_total_ns DESC,to_jsonb(g)::text LIMIT 32", pqxx::params{tx, project});
        Json groups = Json::array();
        for (const auto& row : rows) groups.push_back(parse_row(row));
        const auto total = rows.empty() ? 0 : rows[0]["total_groups"].as<std::int64_t>();
        const auto population = tx.exec("SELECT count(*) AS attempts,count(request_fingerprint) AS captured,"
            "count(*) FILTER(WHERE status=200) AS successes,count(*) FILTER(WHERE status<>200) AS errors,"
            "count(pack_complete) AS known_packs,count(*) FILTER(WHERE pack_complete=false) AS incomplete_packs,"
            "count(returned_records) AS known_results,count(*) FILTER(WHERE returned_records=0) AS zero_results "
            "FROM shin_server.retrieval_journal_attempts WHERE project_id=$1", pqxx::params{tx, project});
        return Json{{"schema_version", query_groups ? "shin.retrieval-journal-queries.v1" : "shin.retrieval-journal-summary.v1"},
            {"metric_contract", "shin.retrieval-journal-metrics.v1"}, {"project_id", project}, {"groups", std::move(groups)},
            {"total_groups", total}, {"omitted_groups", total - static_cast<std::int64_t>(rows.size())},
            {"project_attempts", population[0]["attempts"].as<std::int64_t>()},
            {"project_captured_requests", population[0]["captured"].as<std::int64_t>()},
            {"project_successes", population[0]["successes"].as<std::int64_t>()}, {"project_errors", population[0]["errors"].as<std::int64_t>()},
            {"project_known_packs", population[0]["known_packs"].as<std::int64_t>()},
            {"project_incomplete_packs", population[0]["incomplete_packs"].as<std::int64_t>()},
            {"project_known_results", population[0]["known_results"].as<std::int64_t>()},
            {"project_zero_results", population[0]["zero_results"].as<std::int64_t>()},
            {"query_identity", "exact-captured-payload-only"}, {"coverage", coverage(tx)}, {"read_only", true}};
    });
}
} // namespace shin::server
