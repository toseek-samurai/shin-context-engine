#include "shin/server/store.h"
#include "preparation_access.h"
#include "route_access.h"
#include "legacy_project_access.h"
#include "shin/server/publication_schema.h"
#include "shin/server/identity_directory.h"
#include "shin/server/config.h"

#include <pqxx/pqxx>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace shin::server {
namespace {

constexpr std::size_t kMaxPageSize = 100;
using legacy_access::validate_principal;
using legacy_access::require_admin;
constexpr std::size_t kMaxRetainedJobs = 64;
constexpr std::size_t kMaxPreparationHistory = 65536;
constexpr std::size_t kMaxRetentionInventoryBytes = 32768;
constexpr std::size_t kMaxRetentionInventoryEntries = 64;
constexpr std::uint64_t kMaxReservedArtifactBytes = 1073741824;
constexpr int kMaxLeaseSeconds = 300;

constexpr std::string_view kPublicPreparationColumns =
    "id, project_id, route_id, state, cancel_requested, artifact_snapshot_id, artifact_bytes, "
    "reserved_artifact_bytes, reason_code, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, "
    "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS updated_at";

constexpr std::string_view kFullPreparationColumns =
    "workspace_admission::text AS workspace_admission, "
    "id, project_id, route_id, principal_id, idempotency_key, request_hash, route_fingerprint, expected_snapshot_id, "
    "artifact_namespace, artifact_key, expected_route_revision, "
    "state, cancel_requested, worker_id, "
    "to_char(lease_until AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS lease_until, "
    "artifact_snapshot_id, artifact_bytes, reserved_artifact_bytes, reason_code, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, "
    "to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS updated_at";

constexpr std::string_view kRetentionColumns =
    "preparation_id, state, plan_id, inventory::text AS inventory, requested_by, completed_by, "
    "to_char(requested_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS requested_at, "
    "to_char(released_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS released_at";

[[noreturn]] void invalid_argument() {
    throw ServiceError{400, "invalid-request", "Request contains an invalid value."};
}

[[noreturn]] void invalid_cursor() {
    throw ServiceError{400, "invalid-cursor", "The pagination cursor is invalid."};
}

[[noreturn]] void invalid_limit() {
    throw ServiceError{400, "invalid-limit", "The requested page size is invalid."};
}

[[noreturn]] void database_corrupt() {
    throw ServiceError{500, "database-corrupt", "Stored server data is invalid."};
}

[[noreturn]] void preparation_not_found() {
    throw ServiceError{404, "preparation-not-found", "Preparation was not found."};
}

bool has_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

bool is_public_identifier(std::string_view value) {
    if (value.empty() || value.size() > 63 || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

bool is_idempotency_key(std::string_view value) {
    if (value.empty() || value.size() > 128 || has_nul(value)) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == ':' || character == '-';
    });
}

bool is_sha256_hex(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool is_snapshot_id(std::string_view value) {
    return value.size() == 24 && value.starts_with("fnv1a64:") &&
           std::all_of(value.begin() + 8, value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool is_reason_code(std::string_view value) {
    return is_public_identifier(value);
}

bool is_worker_id(std::string_view value) {
    if (value.size() < 16 || value.size() > 128 || has_nul(value)) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == ':' || character == '-';
    });
}

void validate_job_id(std::uint64_t id) {
    if (id == 0 || id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid_argument();
    }
}

void validate_cursor(std::uint64_t after) {
    if (after > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid_cursor();
    }
}

void validate_limit(std::size_t limit) {
    if (limit == 0 || limit > kMaxPageSize) {
        invalid_limit();
    }
}

void validate_lease_seconds(int lease_seconds) {
    if (lease_seconds < 3 || lease_seconds > kMaxLeaseSeconds) {
        invalid_argument();
    }
}

std::int64_t database_id(std::uint64_t id) {
    validate_job_id(id);
    return static_cast<std::int64_t>(id);
}

template <typename Field>
Json nullable_text(const Field& field) {
    return field.is_null() ? Json(nullptr) : Json(field.template as<std::string>());
}

template <typename Field>
Json nullable_integer(const Field& field) {
    return field.is_null() ? Json(nullptr) : Json(field.template as<std::int64_t>());
}

void require_one_row(const pqxx::result& rows);

template <typename Row>
Json public_preparation_json(const Row& row) {
    return Json{
        {"id", row["id"].template as<std::int64_t>()},
        {"project_id", row["project_id"].template as<std::string>()},
        {"route_id", row["route_id"].template as<std::string>()},
        {"state", row["state"].template as<std::string>()},
        {"cancel_requested", row["cancel_requested"].template as<bool>()},
        {"artifact_snapshot_id", nullable_text(row["artifact_snapshot_id"])},
        {"artifact_bytes", nullable_integer(row["artifact_bytes"])},
        {"reserved_artifact_bytes", row["reserved_artifact_bytes"].template as<std::int64_t>()},
        {"reason_code", nullable_text(row["reason_code"])},
        {"created_at", row["created_at"].template as<std::string>()},
        {"updated_at", row["updated_at"].template as<std::string>()},
    };
}

template <typename Row>
Json full_preparation_json(const Row& row) {
    auto result = public_preparation_json(row);
    result["principal_id"] = row["principal_id"].template as<std::string>();
    result["idempotency_key"] = row["idempotency_key"].template as<std::string>();
    result["request_hash"] = row["request_hash"].template as<std::string>();
    result["route_fingerprint"] = row["route_fingerprint"].template as<std::string>();
    result["expected_snapshot_id"] = row["expected_snapshot_id"].template as<std::string>();
    result["expected_route_revision"] = row["expected_route_revision"].template as<std::int64_t>();
    result["artifact_namespace"] = nullable_text(row["artifact_namespace"]);
    result["artifact_key"] = nullable_text(row["artifact_key"]);
    result["worker_id"] = nullable_text(row["worker_id"]);
    result["lease_until"] = nullable_text(row["lease_until"]);
    result["workspace_admission"] = row["workspace_admission"].is_null() ? Json() :
        parse_bounded_json(row["workspace_admission"].template as<std::string>(), 32768);
    return result;
}

void directory_lock(pqxx::transaction_base& tx) {
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
}

Json authorize_preparation(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                           const WorkspaceReadRequest& scope) {
    return workspace_owner_admission(tx, principal, project, scope);
}

bool current_job_scope(pqxx::transaction_base& tx, const Json& job) {
    const auto& captured = job.at("workspace_admission");
    if (captured.is_null()) return true;
    try {
        const auto& authority = captured.at("authorization");
        const auto& ownership = authority.at("ownership");
        const auto actor_id = job.at("principal_id").get<std::string>();
        if (authority.at("principal_id") != actor_id || ownership.at("project_id") != job.at("project_id")) database_corrupt();
        const Principal actor{actor_id, authority.at("maintenance").get<bool>(), {}};
        const WorkspaceReadRequest scope{ownership.at("tenant_id").get<std::string>(), ownership.at("workspace_id").get<std::string>()};
        return authorize_preparation(tx, actor, job.at("project_id").get<std::string>(), scope) == captured;
    } catch (const ServiceError& error) {
        if (error.status == 403 || error.status == 404) return false;
        throw;
    } catch (const Json::exception&) { database_corrupt(); }
}

bool json_fits(const Json& value, std::size_t maximum_bytes) {
    try {
        return value.dump().size() <= maximum_bytes;
    } catch (const Json::exception&) {
        return false;
    }
}

bool is_bounded_nonnegative_integer(const Json& value, std::uint64_t maximum) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <= maximum;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const auto signed_value = value.get<std::int64_t>();
    return signed_value >= 0 && static_cast<std::uint64_t>(signed_value) <= maximum;
}

bool is_retention_inventory(const Json& inventory) {
    if (!inventory.is_object() || inventory.size() != 3 || !json_fits(inventory, kMaxRetentionInventoryBytes)) {
        return false;
    }
    const auto root_present = inventory.find("root_present");
    const auto entries = inventory.find("entries");
    const auto payload_bytes = inventory.find("payload_bytes");
    return root_present != inventory.end() && root_present->is_boolean() &&
           entries != inventory.end() && entries->is_array() && entries->size() <= kMaxRetentionInventoryEntries &&
           payload_bytes != inventory.end() &&
           is_bounded_nonnegative_integer(*payload_bytes, kMaxReservedArtifactBytes);
}

template <typename Row>
Json retention_json(const Row& row) {
    Json inventory;
    try {
        inventory = Json::parse(row["inventory"].template as<std::string>());
    } catch (const Json::exception&) {
        database_corrupt();
    }
    if (!is_retention_inventory(inventory)) {
        database_corrupt();
    }
    const auto state = row["state"].template as<std::string>();
    const auto plan_id = row["plan_id"].template as<std::string>();
    const auto requested_by = row["requested_by"].template as<std::string>();
    const bool completed_by_is_null = row["completed_by"].is_null();
    const bool released_at_is_null = row["released_at"].is_null();
    if ((state != "intent" && state != "released") || !is_sha256_hex(plan_id) ||
        !is_public_identifier(requested_by) ||
        (!completed_by_is_null && !is_public_identifier(row["completed_by"].template as<std::string>())) ||
        (state == "intent" && (!completed_by_is_null || !released_at_is_null)) ||
        (state == "released" && (completed_by_is_null || released_at_is_null))) {
        database_corrupt();
    }
    return Json{
        {"preparation_id", row["preparation_id"].template as<std::int64_t>()},
        {"state", state},
        {"plan_id", plan_id},
        {"inventory", std::move(inventory)},
        {"requested_by", requested_by},
        {"completed_by", nullable_text(row["completed_by"])},
        {"requested_at", row["requested_at"].template as<std::string>()},
        {"released_at", nullable_text(row["released_at"])},
    };
}

Json current_retention(pqxx::transaction_base& transaction, std::int64_t preparation_id, bool lock) {
    pqxx::params parameters{transaction};
    parameters.append(preparation_id);
    std::string query = "SELECT " + std::string{kRetentionColumns} +
                        " FROM shin_server.preparation_retention WHERE preparation_id = $1";
    if (lock) {
        query += " FOR UPDATE";
    }
    const auto rows = transaction.exec(query, parameters);
    if (rows.empty()) {
        return Json(nullptr);
    }
    require_one_row(rows);
    return retention_json(rows[0]);
}

template <typename Row>
Json inspection_input_json(const Row& row, Json retention) {
    return Json{
        {"job", public_preparation_json(row)},
        {"route_fingerprint", row["route_fingerprint"].template as<std::string>()},
        {"expected_snapshot_id", row["expected_snapshot_id"].template as<std::string>()},
        {"expected_route_revision", row["expected_route_revision"].template as<std::int64_t>()},
        {"artifact_namespace", nullable_text(row["artifact_namespace"])},
        {"artifact_key", nullable_text(row["artifact_key"])},
        {"worker_id", nullable_text(row["worker_id"])},
        {"retention", std::move(retention)},
    };
}

template <typename Row>
std::optional<std::pair<std::string, std::string>> bound_artifact_identity(const Row& row) {
    const bool namespace_is_null = row["artifact_namespace"].is_null();
    const bool key_is_null = row["artifact_key"].is_null();
    if (namespace_is_null && key_is_null) {
        return std::nullopt;
    }
    if (namespace_is_null || key_is_null) {
        database_corrupt();
    }
    const auto artifact_namespace = row["artifact_namespace"].template as<std::string>();
    const auto artifact_key = row["artifact_key"].template as<std::string>();
    if (!is_sha256_hex(artifact_namespace) || !is_sha256_hex(artifact_key)) {
        database_corrupt();
    }
    return std::pair{artifact_namespace, artifact_key};
}

void write_audit_event(pqxx::transaction_base& transaction,
                       std::string_view principal_id,
                       std::string_view action,
                       std::string_view project_id,
                       std::int64_t preparation_id) {
    transaction.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");

    pqxx::params parameters{transaction};
    parameters.append(principal_id);
    parameters.append(action);
    parameters.append(project_id);
    parameters.append(preparation_id);
    transaction.exec(
        "INSERT INTO shin_server.audit_events (principal_id, action, project_id, preparation_id) "
        "VALUES ($1, $2, $3, $4)", parameters);
}

void require_one_row(const pqxx::result& rows) {
    if (rows.size() != 1) {
        database_corrupt();
    }
}

bool terminal_state(std::string_view state) {
    return state == "prepared" || state == "failed" || state == "cancelled";
}

} // namespace

Json Store::preparation_admission(const Principal& principal, const std::string& project, const WorkspaceReadRequest& scope) {
    return run_transaction([&](pqxx::transaction_base& tx) {
        directory_lock(tx);
        return authorize_preparation(tx, principal, project, scope);
    });
}

WorkspaceWriteScope Store::capture_workspace_write(const Principal& principal, const std::string& project,
                                                    const WorkspaceReadRequest& scope) {
    return {project, scope, preparation_admission(principal, project, scope), {}};
}

Json Store::scoped_preparation(const Principal& principal, const std::string& project, std::uint64_t id,
                              const WorkspaceReadRequest& scope) {
    const auto job_id = database_id(id);
    return run_transaction([&](pqxx::transaction_base& tx) {
        directory_lock(tx);
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
        const auto rows = tx.exec("SELECT " + std::string(kPublicPreparationColumns) +
            " FROM shin_server.preparations WHERE id=$1 AND project_id=$2 AND principal_id=$3 "
            "AND scope_tenant_id=$4 AND scope_workspace_id=$5",
            pqxx::params{tx, job_id, project, principal.id, scope.tenant_id, scope.workspace_id});
        if (rows.empty()) preparation_not_found();
        return public_preparation_json(rows[0]);
    });
}

Json Store::scoped_preparations(const Principal& principal, const std::string& project, std::uint64_t after,
                               std::size_t limit, const WorkspaceReadRequest& scope) {
    validate_cursor(after);
    validate_limit(limit);
    return run_transaction([&](pqxx::transaction_base& tx) {
        directory_lock(tx);
        (void)authorize_identity_workspace(tx, principal, scope.tenant_id, project, scope.workspace_id);
        const auto rows = tx.exec("SELECT " + std::string(kPublicPreparationColumns) +
            " FROM shin_server.preparations WHERE id>$1 AND project_id=$2 AND principal_id=$3 "
            "AND scope_tenant_id=$4 AND scope_workspace_id=$5 ORDER BY id LIMIT $6",
            pqxx::params{tx, after, project, principal.id, scope.tenant_id, scope.workspace_id, limit + 1});
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);
        Json items = Json::array();
        for (pqxx::result::size_type i = 0; i < rows.size() && i < result_limit; ++i) items.push_back(public_preparation_json(rows[i]));
        return Json{{"items", items}, {"next_cursor", rows.size() > result_limit ? items.back()["id"] : Json(nullptr)}};
    });
}

Json Store::enqueue_preparation(const Principal& principal,
                                const std::string& project_id,
                                const std::string& route_id,
                                const std::string& route_fingerprint,
                                const std::string& expected_snapshot_id,
                                std::string_view key,
                                std::string_view request_hash,
                                std::size_t max_retained_jobs,
                                std::uint64_t artifact_reservation_bytes,
                                std::string_view artifact_namespace,
                                const std::function<std::string()>& artifact_key_factory,
                                const std::optional<WorkspaceReadRequest>& scope, const Json& admission) {
    validate_principal(principal);
    if (!scope) require_admin(principal);
    if (scope && (!admission.is_object() || !json_fits(admission, 32768))) invalid_argument();
    if (!is_public_identifier(project_id) || !is_public_identifier(route_id) || !is_sha256_hex(route_fingerprint) ||
        (!expected_snapshot_id.empty() && !is_snapshot_id(expected_snapshot_id)) || !is_idempotency_key(key) ||
        !is_sha256_hex(request_hash) || max_retained_jobs == 0 || max_retained_jobs > kMaxRetainedJobs ||
        artifact_reservation_bytes == 0 || artifact_reservation_bytes > kMaxReservedArtifactBytes ||
        !is_sha256_hex(artifact_namespace)) {
        invalid_argument();
    }

    const std::string idempotency_key{key};
    const std::string hash{request_hash};
    const std::string namespace_value{artifact_namespace};
    return run_transaction([&](pqxx::transaction_base& transaction) {
        if (scope) {
            directory_lock(transaction);
            if (authorize_preparation(transaction, principal, project_id, *scope) != admission)
                throw ServiceError(409, "preparation-scope-changed", "Workspace authority changed before enqueue.");
        } else legacy_preparation_scope(transaction, principal);
        transaction.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.preparation-admission.v1'))");

        pqxx::params existing_parameters{transaction};
        existing_parameters.append(principal.id);
        existing_parameters.append(idempotency_key);
        std::string replay_scope = " AND workspace_admission IS NULL";
        if (scope) {
            existing_parameters.append(project_id);
            existing_parameters.append(scope->tenant_id);
            existing_parameters.append(scope->workspace_id);
            replay_scope = " AND workspace_admission IS NOT NULL AND project_id=$3 "
                "AND scope_tenant_id=$4 AND scope_workspace_id=$5";
        }
        const auto existing = transaction.exec(
            "SELECT request_hash, workspace_admission::text AS workspace_admission, " + std::string{kPublicPreparationColumns} +
                " FROM shin_server.preparations WHERE principal_id = $1 AND idempotency_key = $2" + replay_scope,
            existing_parameters);
        if (!existing.empty()) {
            require_one_row(existing);
            const auto captured = existing[0]["workspace_admission"].is_null() ? Json() :
                parse_bounded_json(existing[0]["workspace_admission"].as<std::string>(), 32768);
            if (existing[0]["request_hash"].as<std::string>() != hash || captured != (scope ? admission : Json())) {
                throw ServiceError{409, "idempotency-conflict",
                                   "The idempotency key was already used for a different request."};
            }
            return Json{{"job", public_preparation_json(existing[0])}, {"replayed", true}};
        }

        pqxx::params project_parameters{transaction};
        project_parameters.append(project_id);
        const auto project = transaction.exec("SELECT id FROM shin_server.projects WHERE id = $1", project_parameters);
        if (project.empty()) {
            throw ServiceError{404, "project-not-found", "Project was not found."};
        }
        require_one_row(project);

        const auto retained = preparation_capacity(transaction);
        require_one_row(retained);
        if (retained[0]["history"].as<std::int64_t>() >= static_cast<std::int64_t>(kMaxPreparationHistory)) {
            throw ServiceError{409, "preparation-history-capacity", "The preparation history limit was reached."};
        }
        if (retained[0]["retained"].as<std::int64_t>() >= static_cast<std::int64_t>(max_retained_jobs)) {
            throw ServiceError{409, "preparation-capacity", "The retained preparation limit was reached."};
        }
        const auto reserved = retained[0]["reserved"].as<std::int64_t>();
        const auto available = static_cast<std::int64_t>(kMaxReservedArtifactBytes) - reserved;
        if (reserved < 0 || available < static_cast<std::int64_t>(artifact_reservation_bytes)) {
            throw ServiceError{409, "preparation-capacity", "The preparation artifact reservation limit was reached."};
        }

        // Existing idempotency records and rejected admissions never need new random material.
        if (!artifact_key_factory) invalid_argument();
        const auto key_value = artifact_key_factory();
        if (!is_sha256_hex(key_value)) invalid_argument();

        pqxx::params insert_parameters{transaction};
        insert_parameters.append(project_id);
        insert_parameters.append(route_id);
        insert_parameters.append(principal.id);
        insert_parameters.append(idempotency_key);
        insert_parameters.append(hash);
        insert_parameters.append(route_fingerprint);
        insert_parameters.append(expected_snapshot_id);
        insert_parameters.append(static_cast<std::int64_t>(artifact_reservation_bytes));
        insert_parameters.append(namespace_value);
        insert_parameters.append(key_value);
        pqxx::params base_parameters{transaction, project_id, route_id};
        if (scope) select_content_route(transaction, route_id);
        const auto base = transaction.exec("SELECT revision FROM shin_server.active_routes WHERE project_id=$1 AND route_id=$2", base_parameters);
        insert_parameters.append(base.empty() ? std::int64_t{0} : base[0][0].as<std::int64_t>());
        insert_parameters.append(scope ? std::optional(admission.dump()) : std::nullopt);
        const auto inserted = transaction.exec(
            "INSERT INTO shin_server.preparations "
            "(project_id, route_id, principal_id, idempotency_key, request_hash, route_fingerprint, expected_snapshot_id, "
            "reserved_artifact_bytes, artifact_namespace, artifact_key, expected_route_revision, workspace_admission, state) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12::jsonb, 'queued') RETURNING " +
                std::string{kPublicPreparationColumns},
            insert_parameters);
        require_one_row(inserted);
        write_audit_event(transaction, principal.id, "preparation.queued", project_id,
                          inserted[0]["id"].as<std::int64_t>());
        return Json{{"job", public_preparation_json(inserted[0])}, {"replayed", false}};
    });
}

Json Store::get_preparation(const Principal& principal, std::uint64_t id) {
    validate_principal(principal);
    const auto job_id = database_id(id);
    if (!principal.admin && principal.project_ids.empty()) {
        preparation_not_found();
    }

    return run_transaction([&](pqxx::transaction_base& transaction) {
        legacy_preparation_scope(transaction, principal);
        pqxx::params parameters{transaction};
        parameters.append(job_id);
        std::string query = "SELECT " + std::string{kPublicPreparationColumns} +
                            " FROM shin_server.preparations WHERE id = $1";
        if (!principal.admin) {
            parameters.append(principal.project_ids);
            query += " AND project_id = ANY($2::text[])";
        }
        const auto rows = transaction.exec(query, parameters);
        if (rows.empty()) {
            preparation_not_found();
        }
        require_one_row(rows);
        return public_preparation_json(rows[0]);
    });
}

Json Store::preparation_inspection_input(const Principal& principal, std::uint64_t id,
                                        const std::optional<WorkspaceWriteScope>& write) {
    validate_principal(principal);
    const auto job_id = database_id(id);
    if (!write && !principal.admin && principal.project_ids.empty()) preparation_not_found();
    return run_transaction([&](pqxx::transaction_base& transaction) {
        if (write) {
            authorize_workspace_write(transaction, principal, write->project_id, *write);
            require_scoped_candidate(transaction, principal, id, *write);
        } else legacy_preparation_scope(transaction, principal);
        pqxx::params parameters{transaction};
        parameters.append(job_id);
        std::string query = "SELECT " + std::string{kFullPreparationColumns} +
                            " FROM shin_server.preparations WHERE id = $1";
        if (!write && !principal.admin) {
            parameters.append(principal.project_ids);
            query += " AND project_id = ANY($2::text[])";
        }
        const auto rows = transaction.exec(query, parameters);
        if (rows.empty()) preparation_not_found();
        require_one_row(rows);
        return inspection_input_json(rows[0], current_retention(transaction, job_id, false));
    });
}

Json Store::preparation_cleanup_input(const Principal& principal, std::uint64_t id,
                                     const std::optional<WorkspaceWriteScope>& write) {
    if (!write) return preparation_inspection_input(principal, id);
    validate_principal(principal);
    const auto job_id = database_id(id);
    return run_transaction([&](pqxx::transaction_base& transaction) {
        authorize_preparation_cleanup(transaction, principal, id, *write);
        const auto rows = transaction.exec("SELECT " + std::string{kFullPreparationColumns} +
            " FROM shin_server.preparations WHERE id=$1", pqxx::params{transaction, job_id});
        if (rows.empty()) preparation_not_found();
        require_one_row(rows);
        return inspection_input_json(rows[0], current_retention(transaction, job_id, false));
    });
}

Json Store::list_preparations(const Principal& principal, std::uint64_t after, std::size_t limit) {
    validate_principal(principal);
    validate_cursor(after);
    validate_limit(limit);
    if (!principal.admin && principal.project_ids.empty()) {
        return Json{{"items", Json::array()}, {"next_cursor", nullptr}};
    }

    return run_transaction([&](pqxx::transaction_base& transaction) {
        legacy_preparation_scope(transaction, principal);
        pqxx::params parameters{transaction};
        parameters.append(static_cast<std::int64_t>(after));
        std::string query = "SELECT " + std::string{kPublicPreparationColumns} +
                            " FROM shin_server.preparations WHERE id > $1";
        if (!principal.admin) {
            parameters.append(principal.project_ids);
            query += " AND project_id = ANY($2::text[])";
        }
        query += " ORDER BY id ASC LIMIT $" + std::to_string(principal.admin ? 2 : 3);
        parameters.append(static_cast<std::int64_t>(limit + 1));
        const auto rows = transaction.exec(query, parameters);
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);

        Json items = Json::array();
        const auto returned = std::min(rows.size(), result_limit);
        for (pqxx::result::size_type index = 0; index < returned; ++index) {
            items.push_back(public_preparation_json(rows[index]));
        }
        Json next_cursor = nullptr;
        if (rows.size() > result_limit) {
            next_cursor = rows[result_limit - 1]["id"].as<std::int64_t>();
        }
        return Json{{"items", std::move(items)}, {"next_cursor", std::move(next_cursor)}};
    });
}

Json Store::cancel_preparation(const Principal& principal, std::uint64_t id,
                               const std::optional<WorkspaceWriteScope>& write) {
    validate_principal(principal);
    if (!write) require_admin(principal);
    const auto job_id = database_id(id);

    return run_transaction([&](pqxx::transaction_base& transaction) {
        if (write) authorize_preparation_cleanup(transaction, principal, id, *write);
        else legacy_preparation_scope(transaction, principal);
        pqxx::params select_parameters{transaction};
        select_parameters.append(job_id);
        const auto selected = transaction.exec(
            "SELECT " + std::string{kFullPreparationColumns} +
                " FROM shin_server.preparations WHERE id = $1 FOR UPDATE",
            select_parameters);
        if (selected.empty()) {
            preparation_not_found();
        }
        require_one_row(selected);
        const auto state = selected[0]["state"].as<std::string>();
        if (terminal_state(state) || (state == "running" && selected[0]["cancel_requested"].as<bool>())) {
            return public_preparation_json(selected[0]);
        }
        if (state != "queued" && state != "running") {
            database_corrupt();
        }

        pqxx::params update_parameters{transaction};
        update_parameters.append(job_id);
        pqxx::result updated;
        if (state == "queued") {
            updated = transaction.exec(
                "UPDATE shin_server.preparations SET state = 'cancelled', cancel_requested = TRUE, lease_until = NULL, "
                "reason_code = 'cancelled', updated_at = clock_timestamp() "
                "WHERE id = $1 AND state = 'queued' RETURNING " +
                    std::string{kFullPreparationColumns},
                update_parameters);
        } else {
            updated = transaction.exec(
                "UPDATE shin_server.preparations SET cancel_requested = TRUE, updated_at = clock_timestamp() "
                "WHERE id = $1 AND state = 'running' AND NOT cancel_requested RETURNING " +
                    std::string{kFullPreparationColumns},
                update_parameters);
        }
        require_one_row(updated);

        const auto action = state == "queued" ? "preparation.cancelled" : "preparation.cancel-requested";
        write_audit_event(transaction, principal.id, action, updated[0]["project_id"].as<std::string>(),
                          updated[0]["id"].as<std::int64_t>());
        return public_preparation_json(updated[0]);
    });
}

Json Store::begin_retention(const Principal& principal,
                            std::uint64_t id,
                            const Json& expected_input,
                            const std::string& plan_id,
                            const Json& inventory,
                            const std::optional<WorkspaceWriteScope>& write) {
    validate_principal(principal);
    if (!write) require_admin(principal);
    const auto job_id = database_id(id);
    if (!is_sha256_hex(plan_id) || !expected_input.is_object() ||
        !json_fits(expected_input, kMaxRetentionInventoryBytes) || !is_retention_inventory(inventory)) {
        invalid_argument();
    }

    return run_transaction([&](pqxx::transaction_base& transaction) {
        if (write) authorize_preparation_cleanup(transaction, principal, id, *write);
        else legacy_preparation_scope(transaction, principal);
        pqxx::params select_parameters{transaction};
        select_parameters.append(job_id);
        const auto selected = transaction.exec(
            "SELECT " + std::string{kFullPreparationColumns} +
                " FROM shin_server.preparations WHERE id = $1 FOR UPDATE",
            select_parameters);
        if (selected.empty()) {
            preparation_not_found();
        }
        require_one_row(selected);
        const auto& preparation = selected[0];
        if (preparation_has_active_route(transaction, job_id) ||
            publication_references_preparation(transaction, id))
            throw ServiceError(409, "candidate-active", "An active generation cannot be retained for deletion.");
        if (!terminal_state(preparation["state"].as<std::string>())) {
            throw ServiceError{409, "retention-job-not-terminal", "Retention requires a terminal preparation."};
        }
        if (!bound_artifact_identity(preparation).has_value()) {
            throw ServiceError{409, "retention-artifact-unbound", "Retention requires a bound preparation artifact."};
        }

        const auto retention = current_retention(transaction, job_id, true);
        if (!retention.is_null()) {
            if (retention.at("plan_id").get<std::string>() != plan_id) {
                throw ServiceError{409, "retention-plan-conflict", "Retention uses a different plan."};
            }
            return retention;
        }

        if (inspection_input_json(preparation, nullptr) != expected_input) {
            throw ServiceError{409, "retention-job-changed", "Preparation changed before retention was requested."};
        }

        pqxx::params insert_parameters{transaction};
        insert_parameters.append(job_id);
        insert_parameters.append(plan_id);
        insert_parameters.append(inventory.dump());
        insert_parameters.append(principal.id);
        const auto inserted = transaction.exec(
            "INSERT INTO shin_server.preparation_retention "
            "(preparation_id, state, plan_id, inventory, requested_by) "
            "VALUES ($1, 'intent', $2, $3::jsonb, $4) RETURNING " + std::string{kRetentionColumns},
            insert_parameters);
        require_one_row(inserted);
        write_audit_event(transaction, principal.id, "preparation.retention-requested",
                          preparation["project_id"].as<std::string>(), job_id);
        return retention_json(inserted[0]);
    });
}

Json Store::finish_retention(const Principal& principal,
                             std::uint64_t id,
                             const std::string& plan_id,
                             const std::string& artifact_namespace,
                             const std::string& artifact_key,
                             const std::optional<WorkspaceWriteScope>& write) {
    validate_principal(principal);
    if (!write) require_admin(principal);
    const auto job_id = database_id(id);
    if (!is_sha256_hex(plan_id) || !is_sha256_hex(artifact_namespace) || !is_sha256_hex(artifact_key)) {
        invalid_argument();
    }

    return run_transaction([&](pqxx::transaction_base& transaction) {
        if (write) authorize_preparation_cleanup(transaction, principal, id, *write);
        else legacy_preparation_scope(transaction, principal);
        pqxx::params select_parameters{transaction};
        select_parameters.append(job_id);
        const auto selected = transaction.exec(
            "SELECT " + std::string{kFullPreparationColumns} +
                " FROM shin_server.preparations WHERE id = $1 FOR UPDATE",
            select_parameters);
        if (selected.empty()) {
            preparation_not_found();
        }
        require_one_row(selected);
        const auto& preparation = selected[0];
        const auto binding = bound_artifact_identity(preparation);
        if (preparation_has_active_route(transaction, job_id) ||
            publication_references_preparation(transaction, id))
            throw ServiceError(409, "candidate-active", "An active generation cannot be released.");
        if (!binding.has_value()) {
            throw ServiceError{409, "retention-artifact-unbound", "Retention requires a bound preparation artifact."};
        }
        if (binding->first != artifact_namespace || binding->second != artifact_key) {
            throw ServiceError{409, "retention-job-changed", "Preparation artifact binding changed before retention finished."};
        }
        if (!terminal_state(preparation["state"].as<std::string>()))
            throw ServiceError{409, "retention-job-not-terminal", "Retention requires a terminal preparation."};

        const auto retention = current_retention(transaction, job_id, true);
        if (retention.is_null()) {
            throw ServiceError{409, "retention-not-requested", "Retention was not requested for this preparation."};
        }
        if (retention.at("plan_id").get<std::string>() != plan_id) {
            throw ServiceError{409, "retention-plan-conflict", "Retention uses a different plan."};
        }
        const auto state = retention.at("state").get<std::string>();
        if (state == "released") {
            return retention;
        }
        if (state != "intent") {
            database_corrupt();
        }

        pqxx::params update_parameters{transaction};
        update_parameters.append(job_id);
        update_parameters.append(principal.id);
        const auto released = transaction.exec(
            "UPDATE shin_server.preparation_retention SET state = 'released', completed_by = $2, "
            "released_at = clock_timestamp() WHERE preparation_id = $1 AND state = 'intent' RETURNING " +
                std::string{kRetentionColumns},
            update_parameters);
        require_one_row(released);
        write_audit_event(transaction, principal.id, "preparation.retention-released",
                          preparation["project_id"].as<std::string>(), job_id);
        return retention_json(released[0]);
    });
}

Json Store::claim_preparation(const std::string& worker_id, int lease_seconds) {
    if (!is_worker_id(worker_id)) {
        invalid_argument();
    }
    validate_lease_seconds(lease_seconds);

    return run_transaction([&](pqxx::transaction_base& transaction) {
        directory_lock(transaction);
        preparation_worker_scope(transaction);
        pqxx::params parameters{transaction};
        parameters.append(worker_id);
        parameters.append(lease_seconds);
        const auto claimed = transaction.exec(
            "WITH next_preparation AS ("
            "SELECT id AS candidate_id FROM shin_server.preparations "
            "WHERE state = 'queued' ORDER BY id ASC FOR UPDATE SKIP LOCKED LIMIT 1"
            ") "
            "UPDATE shin_server.preparations AS preparation "
            "SET state = 'running', worker_id = $1, "
            "lease_until = clock_timestamp() + ($2::integer * INTERVAL '1 second'), updated_at = clock_timestamp() "
            "FROM next_preparation WHERE preparation.id = next_preparation.candidate_id RETURNING " +
                std::string{kFullPreparationColumns},
            parameters);
        if (claimed.empty()) {
            return Json(nullptr);
        }
        require_one_row(claimed);
        const auto job = full_preparation_json(claimed[0]);
        if (!current_job_scope(transaction, job)) {
            transaction.exec("UPDATE shin_server.preparations SET state='failed',worker_id=NULL,lease_until=NULL,reason_code='preparation-scope-changed' "
                "WHERE id=$1", pqxx::params{transaction, job.at("id").get<std::int64_t>()});
            write_audit_event(transaction, job.at("principal_id").get<std::string>(), "preparation.failed",
                              job.at("project_id").get<std::string>(), job.at("id").get<std::int64_t>());
            return Json(nullptr);
        }
        write_audit_event(transaction, claimed[0]["principal_id"].as<std::string>(), "preparation.running",
                          claimed[0]["project_id"].as<std::string>(), claimed[0]["id"].as<std::int64_t>());
        return job;
    });
}

bool Store::renew_preparation(std::uint64_t id, const std::string& worker_id, int lease_seconds) {
    const auto job_id = database_id(id);
    if (!is_worker_id(worker_id)) {
        invalid_argument();
    }
    validate_lease_seconds(lease_seconds);

    const auto result = run_transaction([&](pqxx::transaction_base& transaction) {
        directory_lock(transaction);
        preparation_worker_scope(transaction);
        const auto scope_rows = transaction.exec("SELECT " + std::string(kFullPreparationColumns) +
            " FROM shin_server.preparations WHERE id=$1 AND state='running' AND worker_id=$2 "
            "AND lease_until>clock_timestamp() FOR UPDATE", pqxx::params{transaction, job_id, worker_id});
        if (scope_rows.empty() || !current_job_scope(transaction, full_preparation_json(scope_rows[0])))
            return Json{{"renewed", false}};
        pqxx::params parameters{transaction};
        parameters.append(job_id);
        parameters.append(worker_id);
        parameters.append(lease_seconds);
        const auto renewed = transaction.exec(
            "UPDATE shin_server.preparations "
            "SET lease_until = clock_timestamp() + ($3::integer * INTERVAL '1 second'), updated_at = clock_timestamp() "
            "WHERE id = $1 AND state = 'running' AND worker_id = $2 AND lease_until > clock_timestamp() "
            "AND NOT cancel_requested RETURNING id",
            parameters);
        if (renewed.size() > 1) {
            database_corrupt();
        }
        return Json{{"renewed", !renewed.empty()}};
    });
    return result.at("renewed").get<bool>();
}

bool Store::finish_preparation(std::uint64_t id,
                               const std::string& worker_id,
                               const std::string& terminal_state_value,
                               const std::string& reason_code,
                               const std::string& artifact_snapshot_id,
                               std::uint64_t artifact_bytes) {
    const auto job_id = database_id(id);
    if (!is_worker_id(worker_id) || !terminal_state(terminal_state_value) ||
        artifact_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid_argument();
    }

    std::string normalized_reason;
    if (terminal_state_value == "prepared") {
        if (!reason_code.empty() || !is_snapshot_id(artifact_snapshot_id)) {
            invalid_argument();
        }
    } else {
        if (!artifact_snapshot_id.empty() || artifact_bytes != 0) {
            invalid_argument();
        }
        if (terminal_state_value == "failed") {
            if (!is_reason_code(reason_code)) {
                invalid_argument();
            }
            normalized_reason = reason_code;
        } else {
            if (!reason_code.empty() && !is_reason_code(reason_code)) {
                invalid_argument();
            }
            normalized_reason = reason_code.empty() ? "cancelled" : reason_code;
        }
    }

    const auto result = run_transaction([&](pqxx::transaction_base& transaction) {
        directory_lock(transaction);
        preparation_worker_scope(transaction);
        if (terminal_state_value == "prepared")
            transaction.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.activation-writers.v1'))");
        pqxx::params ownership_parameters{transaction};
        ownership_parameters.append(job_id);
        ownership_parameters.append(worker_id);
        const auto active = transaction.exec(
            "SELECT " + std::string(kFullPreparationColumns) + " FROM shin_server.preparations "
            "WHERE id = $1 AND state = 'running' AND worker_id = $2 AND lease_until > clock_timestamp() FOR UPDATE",
            ownership_parameters);
        if (active.empty()) {
            return Json{{"finished", false}};
        }
        require_one_row(active);
        auto final_value = terminal_state_value;
        auto final_reason = normalized_reason;
        const auto job = full_preparation_json(active[0]);
        if (final_value == "prepared" && !job.at("workspace_admission").is_null()) {
            if (!current_job_scope(transaction, job)) {
                final_value = "failed";
                final_reason = "preparation-scope-changed";
            } else {
                const auto base = transaction.exec("SELECT revision,snapshot_id FROM shin_server.active_routes WHERE project_id=$1 AND route_id=$2",
                    pqxx::params{transaction, job.at("project_id").get<std::string>(), job.at("route_id").get<std::string>()});
                const auto revision = base.empty() ? std::int64_t{0} : base[0]["revision"].as<std::int64_t>();
                const auto snapshot = base.empty() || base[0]["snapshot_id"].is_null() ? "" : base[0]["snapshot_id"].as<std::string>();
                if (revision != job.at("expected_route_revision").get<std::int64_t>() || snapshot != job.at("expected_snapshot_id").get<std::string>()) {
                    final_value = "failed";
                    final_reason = "preparation-base-changed";
                }
            }
        }
        if (terminal_state_value == "prepared" && !active[0]["cancel_requested"].as<bool>() &&
            artifact_bytes > static_cast<std::uint64_t>(active[0]["reserved_artifact_bytes"].as<std::int64_t>())) {
            invalid_argument();
        }

        pqxx::params parameters{transaction};
        parameters.append(job_id);
        parameters.append(worker_id);
        parameters.append(final_value);
        parameters.append(terminal_state_value == "cancelled");
        parameters.append(artifact_snapshot_id);
        parameters.append(static_cast<std::int64_t>(artifact_bytes));
        parameters.append(final_reason);
        const auto finished = transaction.exec(
            "UPDATE shin_server.preparations "
            "SET state = CASE WHEN cancel_requested THEN 'cancelled' ELSE $3 END, "
            "cancel_requested = cancel_requested OR $4, lease_until = NULL, "
            "artifact_snapshot_id = CASE WHEN cancel_requested OR $3 <> 'prepared' THEN NULL ELSE $5 END, "
            "artifact_bytes = CASE WHEN cancel_requested OR $3 <> 'prepared' THEN NULL ELSE $6 END, "
            "reason_code = CASE WHEN cancel_requested OR $3 = 'cancelled' THEN 'cancelled' "
            "WHEN $3 = 'prepared' THEN NULL ELSE $7 END, updated_at = clock_timestamp() "
            "WHERE id = $1 AND state = 'running' AND worker_id = $2 AND lease_until > clock_timestamp() "
            "AND (cancel_requested OR $3 <> 'prepared' OR $6 <= reserved_artifact_bytes) RETURNING " +
                std::string{kFullPreparationColumns},
            parameters);
        if (finished.empty()) {
            return Json{{"finished", false}};
        }
        require_one_row(finished);
        const auto final_state = finished[0]["state"].as<std::string>();
        if (!terminal_state(final_state)) {
            database_corrupt();
        }
        write_audit_event(transaction, finished[0]["principal_id"].as<std::string>(),
                          std::string{"preparation."} + final_state,
                          finished[0]["project_id"].as<std::string>(), finished[0]["id"].as<std::int64_t>());
        return Json{{"finished", true}};
    });
    return result.at("finished").get<bool>();
}

void Store::expire_preparations() {
    run_transaction([](pqxx::transaction_base& transaction) {
        preparation_worker_scope(transaction);
        const auto expired = transaction.exec(
            "WITH expired_preparations AS ("
            "SELECT id AS candidate_id FROM shin_server.preparations "
            "WHERE state = 'running' AND lease_until <= clock_timestamp() "
            "ORDER BY lease_until ASC, id ASC FOR UPDATE SKIP LOCKED"
            ") "
            "UPDATE shin_server.preparations AS preparation "
            "SET state = CASE WHEN preparation.cancel_requested THEN 'cancelled' ELSE 'failed' END, "
            "lease_until = NULL, artifact_snapshot_id = NULL, artifact_bytes = NULL, "
            "reason_code = CASE WHEN preparation.cancel_requested THEN 'cancelled' ELSE 'lease-expired' END, "
            "updated_at = clock_timestamp() FROM expired_preparations "
            "WHERE preparation.id = expired_preparations.candidate_id RETURNING " +
                std::string{kFullPreparationColumns});
        for (const auto& row : expired) {
            const auto state = row["state"].as<std::string>();
            if (state != "failed" && state != "cancelled") {
                database_corrupt();
            }
            write_audit_event(transaction, row["principal_id"].as<std::string>(),
                              std::string{"preparation."} + state, row["project_id"].as<std::string>(),
                              row["id"].as<std::int64_t>());
        }
        return Json(nullptr);
    });
}

} // namespace shin::server
