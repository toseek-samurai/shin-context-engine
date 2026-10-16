#pragma once

#include "shin/server/model.h"
#include <pqxx/pqxx>

namespace shin::server::audit_detail {

inline constexpr auto columns =
    "id, principal_id, action, project_id, workspace_id, preparation_id, activation_route_id, "
    "activation_previous_revision, observer_stream_id, observer_sequence, publication_proposal_id, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at ";

inline Json record(const pqxx::row_ref& row) {
    Json result;
    for (const auto* key : {"principal_id", "action", "project_id", "workspace_id", "activation_route_id",
                            "observer_stream_id", "created_at"})
        result[key] = row[key].is_null() ? Json(nullptr) : Json(row[key].as<std::string>());
    for (const auto* key : {"id", "preparation_id", "activation_previous_revision", "observer_sequence", "publication_proposal_id"})
        result[key] = row[key].is_null() ? Json(nullptr) : Json(row[key].as<std::int64_t>());
    return result;
}

} // namespace shin::server::audit_detail
