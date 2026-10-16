#include "shin/server/store.h"
#include "publication_store_internal.h"
#include "preparation_access.h"

#include <optional>

namespace shin::server {
namespace {
using publication_store::nullable_id;
using publication_store::nullable_text;

bool same_field(const Json& object, const char* key, const Json& expected) {
    const auto field = object.find(key);
    return field != object.end() && *field == expected;
}

bool matches_verified_input(const pqxx::row_ref& row, const Json& input) {
    if (!input.is_object() || !input.contains("job") || !input.at("job").is_object()) return false;
    const auto& job = input.at("job");
    // Compare the inspection contract to the locked ledger without rebuilding its response envelope.
    return same_field(job, "id", row["id"].as<std::int64_t>()) &&
        same_field(job, "project_id", row["project_id"].as<std::string>()) &&
        same_field(job, "route_id", row["route_id"].as<std::string>()) &&
        same_field(job, "state", row["state"].as<std::string>()) &&
        same_field(job, "cancel_requested", row["cancel_requested"].as<bool>()) &&
        same_field(job, "artifact_snapshot_id", nullable_text(row["artifact_snapshot_id"])) &&
        same_field(job, "artifact_bytes", nullable_id(row["artifact_bytes"])) &&
        same_field(job, "reserved_artifact_bytes", row["reserved_artifact_bytes"].as<std::int64_t>()) &&
        same_field(job, "reason_code", nullable_text(row["reason_code"])) &&
        same_field(job, "created_at", row["created_text"].as<std::string>()) &&
        same_field(job, "updated_at", row["updated_text"].as<std::string>()) &&
        same_field(input, "route_fingerprint", row["route_fingerprint"].as<std::string>()) &&
        same_field(input, "expected_snapshot_id", row["expected_snapshot_id"].as<std::string>()) &&
        same_field(input, "expected_route_revision", row["expected_route_revision"].as<std::int64_t>()) &&
        same_field(input, "artifact_namespace", nullable_text(row["artifact_namespace"])) &&
        same_field(input, "artifact_key", nullable_text(row["artifact_key"])) &&
        same_field(input, "worker_id", nullable_text(row["worker_id"])) &&
        same_field(input, "retention", nullptr);
}

void validate_source_base(pqxx::transaction_base& tx, const std::string& project, const pqxx::row_ref& candidate) {
    preparation_route_source(tx, project, candidate["route_id"].as<std::string>());
    const auto rows = tx.exec(
        "SELECT a.revision,a.snapshot_id,a.preparation_id, "
        "(p.state='prepared' AND NOT p.cancel_requested AND p.artifact_namespace IS NOT NULL AND p.artifact_key IS NOT NULL "
        "AND p.artifact_bytes IS NOT NULL AND a.snapshot_id=p.artifact_snapshot_id AND a.route_fingerprint=p.route_fingerprint "
        "AND NOT EXISTS(SELECT 1 FROM shin_server.preparation_retention r WHERE r.preparation_id=p.id)) AS valid "
        "FROM shin_server.active_routes a LEFT JOIN shin_server.preparations p "
        "ON p.project_id=a.project_id AND p.route_id=a.route_id AND p.id=a.preparation_id "
        "WHERE a.project_id=$1 AND a.route_id=$2", pqxx::params{tx, project, candidate["route_id"].as<std::string>()});
    std::int64_t revision = 0;
    std::string snapshot;
    if (!rows.empty()) {
        const auto& row = rows[0];
        revision = row["revision"].as<std::int64_t>();
        if (!row["preparation_id"].is_null()) {
            if (row["valid"].is_null() || !row["valid"].as<bool>())
                throw ServiceError(500, "database-corrupt", "Source generation binding is invalid.");
            snapshot = row["snapshot_id"].as<std::string>();
        }
    }
    if (candidate["expected_route_revision"].as<std::int64_t>() != revision ||
        candidate["expected_snapshot_id"].as<std::string>() != snapshot)
        publication_store::conflict();
}
} // namespace

Json Store::publication_resolution_replay(const Principal& principal, const std::string& project,
                                          const std::string& publication, std::uint64_t id,
                                          const std::string& decision, std::uint64_t expected_revision,
                                          const std::optional<WorkspaceWriteScope>& write) {
    using namespace publication_store;
    validate_id(id);
    validate_revision(expected_revision);
    validate_decision(decision);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, true, write);
        if (write) tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        require_workspace_history(tx, project, publication, write);
        return replay_resolution(select_proposal(tx, project, publication, id), decision, expected_revision);
    });
}

Json Store::resolve_publication_proposal(const Principal& principal, const std::string& project,
                                         const std::string& publication, std::uint64_t id,
                                         const std::string& decision, std::uint64_t expected_revision,
                                         const Json& verified_input, const std::optional<WorkspaceWriteScope>& write) {
    using namespace publication_store;
    validate_id(id);
    validate_revision(expected_revision);
    validate_decision(decision);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, true, write);
        const bool accept = decision == "accept";
        if (accept) tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.activation-writers.v1'))");
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.publication-writers.v1'))");
        require_workspace_history(tx, project, publication, write);
        const auto detail = select_proposal(tx, project, publication, id);
        const auto existing = replay_resolution(detail, decision, expected_revision);
        if (!existing.is_null()) return existing;
        const auto preparation = detail.at("proposal").at("preparation_id").get<std::uint64_t>();
        std::optional<std::string> snapshot;
        std::optional<std::string> fingerprint;
        if (accept) {
            if (write) require_scoped_candidate(tx, principal, preparation, *write);
            const auto rows = tx.exec(
                "SELECT p.*,s.route_fingerprint AS proposal_fingerprint,s.source_route_id,s.source_host_id, "
                "to_char(p.created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_text, "
                "to_char(p.updated_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS updated_text "
                "FROM shin_server.preparations p JOIN shin_server.publication_proposals s "
                "ON s.project_id=p.project_id AND s.preparation_id=p.id AND s.id=$3 "
                "WHERE p.project_id=$1 AND p.id=$2 FOR UPDATE OF p",
                pqxx::params{tx, project, preparation, id});
            if (rows.size() != 1) throw ServiceError(404, "preparation-not-found", "Preparation was not found.");
            const auto& row = rows[0];
            if (!tx.exec("SELECT 1 FROM shin_server.preparation_retention WHERE preparation_id=$1", pqxx::params{tx, preparation}).empty())
                throw ServiceError(409, "candidate-retention-pending", "Candidate is reserved for retention.");
            if (row["state"].as<std::string>() != "prepared" || row["cancel_requested"].as<bool>() ||
                row["artifact_namespace"].is_null() || row["artifact_key"].is_null() || row["artifact_snapshot_id"].is_null() ||
                row["artifact_bytes"].is_null() || row["route_fingerprint"].as<std::string>() != row["proposal_fingerprint"].as<std::string>() ||
                row["route_id"].as<std::string>() != row["source_route_id"].as<std::string>() ||
                row["source_host_id"].as<std::string>() != source_host_id_ || !matches_verified_input(row, verified_input)) conflict();
            validate_source_base(tx, project, row);
            snapshot = row["artifact_snapshot_id"].as<std::string>();
            fingerprint = row["route_fingerprint"].as<std::string>();
        }

        const auto previous = static_cast<std::int64_t>(expected_revision);
        const auto revision = previous + (accept ? 1 : 0);
        // The head FK binds it to this exact accepted receipt, not merely to a prepared artifact.
        tx.exec("INSERT INTO shin_server.publication_resolutions "
                "(proposal_id,project_id,publication_id,decision,expected_revision,revision,preparation_id,resolved_by) "
                "VALUES($1,$2,$3,$4,$5,$6,$7,$8)",
                pqxx::params{tx, id, project, publication, decision, previous, revision, preparation, principal.id});
        if (accept) {
            tx.exec("INSERT INTO shin_server.publication_heads "
                    "(project_id,publication_id,revision,preparation_id,snapshot_id,route_fingerprint,proposal_id,decision) "
                    "VALUES($1,$2,$3,$4,$5,$6,$7,'accept') ON CONFLICT(project_id,publication_id) DO UPDATE SET "
                    "revision=EXCLUDED.revision,preparation_id=EXCLUDED.preparation_id,snapshot_id=EXCLUDED.snapshot_id, "
                    "route_fingerprint=EXCLUDED.route_fingerprint,proposal_id=EXCLUDED.proposal_id,decision=EXCLUDED.decision",
                    pqxx::params{tx, project, publication, revision, preparation, snapshot, fingerprint, id});
        }
        audit(tx, principal, project, preparation, id,
              accept ? "publication.accepted" : decision == "reject" ? "publication.rejected" : "publication.reprepare-requested");
        return Json{{"replayed", false}, {"receipt", select_proposal(tx, project, publication, id).at("resolution")}};
    });
}
} // namespace shin::server
