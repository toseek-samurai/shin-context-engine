#pragma once

#include "shin/server/model.h"
#include "shin/server/identity_directory.h"

#include <pqxx/pqxx>
#include <cstdint>
#include <string>
#include <string_view>

namespace shin::server::publication_store {
// Every consumer observes both immutable transitions and the active pointer in one snapshot.
inline constexpr std::string_view kHeadCte =
    "WITH publication_state AS (SELECT h.*, "
    "GREATEST(COALESCE(a.revision,0),COALESCE(w.revision,0)) AS logical_revision, "
    "CASE WHEN a.revision IS NULL AND w.revision IS NULL THEN 'empty' "
    "WHEN a.revision>COALESCE(w.revision,0) THEN 'active' ELSE 'withdrawn' END AS head_state, "
    "COALESCE(CASE WHEN a.revision IS NULL AND w.revision IS NULL THEN h.revision IS NULL "
    "WHEN a.revision>COALESCE(w.revision,0) THEN h.revision=a.revision AND h.proposal_id=a.proposal_id "
    "AND h.preparation_id=a.preparation_id "
    "WHEN w.revision>a.revision THEN h.revision IS NULL AND w.previous_proposal_id=a.proposal_id "
    "AND w.expected_revision=a.revision AND w.previous_preparation_id=a.preparation_id "
    "ELSE false END,false) AS head_valid "
    "FROM (SELECT 1) seed "
    "LEFT JOIN LATERAL (SELECT proposal_id,revision,preparation_id FROM shin_server.publication_resolutions "
    "WHERE project_id=$1 AND publication_id=$2 AND decision='accept' ORDER BY revision DESC LIMIT 1) a ON true "
    "LEFT JOIN LATERAL (SELECT revision,expected_revision,previous_proposal_id,previous_preparation_id "
    "FROM shin_server.publication_withdrawals WHERE project_id=$1 AND publication_id=$2 "
    "ORDER BY revision DESC LIMIT 1) w ON true "
    "LEFT JOIN shin_server.publication_heads h ON h.project_id=$1 AND h.publication_id=$2) ";

void validate_head_pointer(const pqxx::row_ref& row);
std::int64_t admission_epoch(pqxx::transaction_base& tx, const std::optional<WorkspaceWriteScope>& write = std::nullopt);
[[noreturn]] void conflict();
void validate_id(std::uint64_t id);
void validate_revision(std::uint64_t revision);
void validate_decision(const std::string& decision);
void authorize(pqxx::transaction_base& tx, const Principal& principal,
               const std::string& project, const std::string& publication, bool admin,
               const std::optional<WorkspaceWriteScope>& write = std::nullopt);
// Called under a publication writer/shared lock, before loading any public detail.
void require_workspace_history(pqxx::transaction_base& tx, const std::string& project,
                               const std::string& publication, const std::optional<WorkspaceWriteScope>& write);
Json select_head(pqxx::transaction_base& tx, const std::string& project, const std::string& publication);
Json select_proposal(pqxx::transaction_base& tx, const std::string& project,
                     const std::string& publication, std::uint64_t id, bool private_review = false);
Json replay_resolution(const Json& detail, const std::string& decision, std::uint64_t expected_revision);
void audit(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
           std::uint64_t preparation, std::uint64_t proposal, const std::string& action);

inline Json nullable_text(const pqxx::field_ref& field) {
    return field.is_null() ? Json(nullptr) : Json(field.as<std::string>());
}
inline Json nullable_id(const pqxx::field_ref& field) {
    return field.is_null() ? Json(nullptr) : Json(field.as<std::int64_t>());
}
} // namespace shin::server::publication_store
