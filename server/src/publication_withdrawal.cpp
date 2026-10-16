#include "shin/server/store.h"
#include "publication_store_internal.h"

namespace shin::server {
namespace {
Json withdrawal_receipt(const pqxx::row_ref& row) {
    return {{"schema_version", "shin.publication-withdrawal-receipt.v1"},
            {"id", row["id"].as<std::int64_t>()},
            {"project_id", row["project_id"].as<std::string>()},
            {"publication_id", row["publication_id"].as<std::string>()},
            {"expected_revision", row["expected_revision"].as<std::int64_t>()},
            {"revision", row["revision"].as<std::int64_t>()},
            {"previous_proposal_id", row["previous_proposal_id"].as<std::int64_t>()},
            {"previous_preparation_id", row["previous_preparation_id"].as<std::int64_t>()},
            {"withdrawn_by", row["withdrawn_by"].as<std::string>()},
            {"created_at", row["created_text"].as<std::string>()}};
}

constexpr std::string_view kReceiptColumns =
    "id,project_id,publication_id,expected_revision,revision,previous_proposal_id,previous_preparation_id,withdrawn_by, "
    "to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_text";
} // namespace

Json Store::withdraw_publication(const Principal& principal, const std::string& project,
                                const std::string& publication, std::uint64_t expected_revision,
                                const std::optional<WorkspaceWriteScope>& write) {
    using namespace publication_store;
    validate_revision(expected_revision);
    return run_transaction([&](pqxx::transaction_base& tx) {
        authorize(tx, principal, project, publication, true, write);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.publication-writers.v1'))");
        require_workspace_history(tx, project, publication, write);
        const auto existing = tx.exec(
            "SELECT " + std::string(kReceiptColumns) + " FROM shin_server.publication_withdrawals "
            "WHERE project_id=$1 AND publication_id=$2 AND expected_revision=$3",
            pqxx::params{tx, project, publication, expected_revision});
        if (!existing.empty()) return Json{{"replayed", true}, {"receipt", withdrawal_receipt(existing[0])}};

        // Revocation checks the logical pointer, never candidate/source/artifact admission.
        const auto heads = tx.exec(std::string(kHeadCte) + "SELECT * FROM publication_state",
                                   pqxx::params{tx, project, publication});
        const auto& head = heads[0];
        validate_head_pointer(head);
        if (head["head_state"].as<std::string>() != "active" ||
            head["logical_revision"].as<std::uint64_t>() != expected_revision) conflict();
        const auto proposal = head["proposal_id"].as<std::uint64_t>();
        const auto preparation = head["preparation_id"].as<std::uint64_t>();
        const auto inserted = tx.exec(
            "INSERT INTO shin_server.publication_withdrawals "
            "(project_id,publication_id,expected_revision,revision,previous_proposal_id,previous_preparation_id,withdrawn_by) "
            "VALUES($1,$2,$3,$4,$5,$6,$7) RETURNING " + std::string(kReceiptColumns),
            pqxx::params{tx, project, publication, expected_revision, expected_revision + 1, proposal, preparation, principal.id});
        const auto removed = tx.exec(
            "DELETE FROM shin_server.publication_heads WHERE project_id=$1 AND publication_id=$2 "
            "AND revision=$3 AND proposal_id=$4 RETURNING proposal_id",
            pqxx::params{tx, project, publication, expected_revision, proposal});
        if (removed.size() != 1) conflict();
        audit(tx, principal, project, preparation, proposal, "publication.withdrawn");
        return Json{{"replayed", false}, {"receipt", withdrawal_receipt(inserted[0])}};
    });
}
} // namespace shin::server
