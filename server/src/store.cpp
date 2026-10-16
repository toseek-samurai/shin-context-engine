#include "shin/server/store.h"
#include "shin/server/identity.h"
#include "store_pool.h"
#include "store_session.h"
#include "audit_serialization.h"
#include "legacy_project_access.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shin::server {
namespace {

constexpr std::size_t kMaxPageSize = 100;
using legacy_access::validate_principal;
using legacy_access::require_admin;
using legacy_access::principal_can_access;

constexpr std::string_view kDatabaseCorruptError = "Stored server data is invalid.";

[[noreturn]] void throw_invalid_argument() {
    throw ServiceError{400, "invalid-request", "Request contains an invalid value."};
}

[[noreturn]] void throw_invalid_cursor() {
    throw ServiceError{400, "invalid-cursor", "The pagination cursor is invalid."};
}

[[noreturn]] void throw_invalid_limit() {
    throw ServiceError{400, "invalid-limit", "The requested page size is invalid."};
}

[[noreturn]] void throw_database_corrupt() {
    throw ServiceError{500, "database-corrupt", std::string{kDatabaseCorruptError}};
}

bool has_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

void require_bounded(std::string_view value, std::size_t max_size) {
    if (value.empty() || value.size() > max_size || has_nul(value)) {
        throw_invalid_argument();
    }
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
    if (value.empty() || value.size() > 128) {
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

void validate_project_input(const ProjectInput& project) {
    if (!is_public_identifier(project.id)) {
        throw_invalid_argument();
    }
    require_bounded(project.display_name, 120);
}

void validate_cursor(std::string_view cursor) {
    if (!cursor.empty() && !is_public_identifier(cursor)) {
        throw_invalid_cursor();
    }
}

void validate_limit(std::size_t limit) {
    if (limit == 0 || limit > kMaxPageSize) {
        throw_invalid_limit();
    }
}

void validate_audit_cursor(std::uint64_t after) {
    if (after > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw_invalid_cursor();
    }
}

template <typename Row>
Json project_json(const Row& row) {
    return Json{
        {"id", row["id"].template as<std::string>()},
        {"display_name", row["display_name"].template as<std::string>()},
        {"created_at", row["created_at"].template as<std::string>()},
        {"revision", row["revision"].template as<std::int64_t>()},
    };
}


} // namespace

Store::Store(StoreConfig config)
    : source_host_id_(config.source_host_id), impl_(std::make_unique<Impl>(std::move(config))) {}

Store::~Store() = default;

Json Store::run_transaction(std::function<Json(pqxx::transaction_base&)> operation) {
    try {
        return impl_->execute([&operation](pqxx::connection& connection) {
            pqxx::work transaction{connection};
            auto result = operation(transaction);
            transaction.commit();
            return result;
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

Json Store::readiness() {
    try {
        return impl_->execute([&](pqxx::connection& connection) {
            pqxx::read_transaction tx{connection};
            const auto id = read_server_identity(tx);
            tx.commit();
            return Json{{"status", "ready"}, {"scope", "store"}, {"server_id", id}};
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

RegistrationResult Store::register_project(const Principal& principal,
                                           const ProjectInput& project,
                                           std::string_view idempotency_key,
                                           std::string_view request_hash) {
    validate_principal(principal);
    require_admin(principal);
    validate_project_input(project);
    if (!is_idempotency_key(idempotency_key)) {
        throw ServiceError{400, "invalid-idempotency-key", "A valid Idempotency-Key is required."};
    }
    if (!is_sha256_hex(request_hash)) {
        throw_invalid_argument();
    }

    try {
        return impl_->execute([&](pqxx::connection& connection) {
            pqxx::work transaction{connection};
            const std::string key{idempotency_key};
            const std::string hash{request_hash};

            pqxx::params lock_parameters{transaction};
            lock_parameters.append(principal.id);
            lock_parameters.append(key);
            transaction.exec(
                "SELECT pg_advisory_xact_lock(hashtext($1), hashtext($2))", lock_parameters);

            pqxx::params existing_parameters{transaction};
            existing_parameters.append(principal.id);
            existing_parameters.append(key);
            const auto existing = transaction.exec(
                "SELECT request_hash, project::text AS project "
                "FROM shin_server.idempotency_requests "
                "WHERE principal_id = $1 AND key = $2",
                existing_parameters);
            if (!existing.empty()) {
                if (existing.size() != 1 || existing[0]["request_hash"].as<std::string>() != hash) {
                    throw ServiceError{409, "idempotency-conflict",
                                       "The idempotency key was already used for a different request."};
                }
                const auto stored_project = Json::parse(existing[0]["project"].as<std::string>());
                if (!stored_project.is_object()) {
                    throw_database_corrupt();
                }
                transaction.commit();
                return RegistrationResult{stored_project, true};
            }

            pqxx::params project_parameters{transaction};
            project_parameters.append(project.id);
            project_parameters.append(project.display_name);
            const auto inserted_project = transaction.exec(
                "INSERT INTO shin_server.projects (id, display_name) "
                "VALUES ($1, $2) "
                "RETURNING id, display_name, "
                "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, "
                "revision",
                project_parameters);
            if (inserted_project.size() != 1) {
                throw_database_corrupt();
            }
            const auto response_project = project_json(inserted_project[0]);

            // Every public audit writer takes this lock before allocating an identity value.
            // Holding it through commit makes the durable cursor follow commit order.
            transaction.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");

            pqxx::params audit_parameters{transaction};
            audit_parameters.append(principal.id);
            audit_parameters.append("project.registered");
            audit_parameters.append(project.id);
            transaction.exec(
                "INSERT INTO shin_server.audit_events (principal_id, action, project_id, preparation_id) "
                "VALUES ($1, $2, $3, NULL)",
                audit_parameters);

            pqxx::params receipt_parameters{transaction};
            receipt_parameters.append(principal.id);
            receipt_parameters.append(key);
            receipt_parameters.append(hash);
            receipt_parameters.append(response_project.dump());
            transaction.exec(
                "INSERT INTO shin_server.idempotency_requests (principal_id, key, request_hash, project) "
                "VALUES ($1, $2, $3, $4::jsonb)",
                receipt_parameters);

            transaction.commit();
            return RegistrationResult{response_project, false};
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::unique_violation&) {
        throw ServiceError{409, "project-conflict", "A project with this id already exists."};
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

Json Store::list_projects(const Principal& principal, std::string_view after, std::size_t limit) {
    validate_principal(principal);
    validate_cursor(after);
    validate_limit(limit);

    try {
        return impl_->execute([&](pqxx::connection& connection) {
            if (!principal.admin && principal.project_ids.empty()) {
                return Json{{"items", Json::array()}, {"next_cursor", nullptr}};
            }
            pqxx::read_transaction transaction{connection};
            pqxx::params parameters{transaction};
            parameters.append(std::string{after});
            std::string query;
            if (principal.admin) {
                query =
                    "SELECT id, display_name, "
                    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, revision "
                    "FROM shin_server.projects WHERE id > $1 ORDER BY id ASC LIMIT $2";
            } else {
                parameters.append(principal.project_ids);
                query =
                    "SELECT id, display_name, "
                    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, revision "
                    "FROM shin_server.projects WHERE id > $1 AND id = ANY($2::text[]) "
                    "ORDER BY id ASC LIMIT $3";
            }
            parameters.append(static_cast<std::int64_t>(limit + 1));
            const auto rows = transaction.exec(query, parameters);
            const auto result_limit = static_cast<pqxx::result::size_type>(limit);

            Json items = Json::array();
            const auto returned = std::min(rows.size(), result_limit);
            for (pqxx::result::size_type index = 0; index < returned; ++index) {
                items.push_back(project_json(rows[index]));
            }
            Json next_cursor = nullptr;
            if (rows.size() > result_limit) {
                next_cursor = rows[result_limit - 1]["id"].as<std::string>();
            }
            transaction.commit();
            return Json{{"items", std::move(items)}, {"next_cursor", std::move(next_cursor)}};
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

Json Store::get_project(const Principal& principal, std::string_view id) {
    validate_principal(principal);
    if (!is_public_identifier(id)) {
        throw_invalid_argument();
    }
    if (!principal_can_access(principal, id)) {
        throw ServiceError{404, "project-not-found", "Project was not found."};
    }

    try {
        return impl_->execute([&](pqxx::connection& connection) {
            pqxx::read_transaction transaction{connection};
            pqxx::params parameters{transaction};
            parameters.append(std::string{id});
            const auto rows = transaction.exec(
                "SELECT id, display_name, "
                "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at, revision "
                "FROM shin_server.projects WHERE id = $1",
                parameters);
            if (rows.empty()) {
                throw ServiceError{404, "project-not-found", "Project was not found."};
            }
            if (rows.size() != 1) {
                throw_database_corrupt();
            }
            const auto project = project_json(rows[0]);
            transaction.commit();
            return project;
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

Json Store::audit_events(const Principal& principal, std::uint64_t after, std::size_t limit) {
    validate_principal(principal);
    validate_audit_cursor(after);
    validate_limit(limit);

    try {
        return impl_->execute([&](pqxx::connection& connection) {
            if (!principal.admin && principal.project_ids.empty()) {
                return Json{{"items", Json::array()}, {"next_cursor", nullptr}};
            }
            pqxx::read_transaction transaction{connection};
            // Retention and every audit writer share this fence; never silently skip a retired prefix.
            transaction.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_ops','1',true)");
            const auto history = transaction.exec("SELECT retired_through FROM shin_server.audit_history WHERE singleton");
            if (history.size() != 1) throw_database_corrupt();
            if (after < history[0][0].as<std::uint64_t>())
                throw ServiceError(410, "audit-history-retired", "Audit cursor predates retained history. Request a reviewed current cursor from an administrator.");
            std::string projects;
            for (const auto& project : principal.project_ids) {
                if (!projects.empty()) projects += ',';
                projects += project;
            }
            transaction.exec("SELECT set_config('shin.audit_ops',$1,true),set_config('shin.audit_projects',$2,true),"
                "set_config('shin.observer_legacy','1',true),set_config('shin.identity_principal',$3,true),"
                "set_config('shin.identity_legacy_projects',$2,true)",
                pqxx::params{transaction, principal.admin ? "1" : "0", projects, principal.id});
            pqxx::params parameters{transaction};
            parameters.append(static_cast<std::int64_t>(after));
            std::string query;
            if (principal.admin) {
                query = std::string("SELECT ") + audit_detail::columns +
                    "FROM shin_server.audit_events WHERE id > $1 ORDER BY id ASC LIMIT $2";
            } else {
                parameters.append(principal.project_ids);
                parameters.append(principal.id);
                query = std::string("SELECT ") + audit_detail::columns +
                    "FROM shin_server.audit_events WHERE id > $1 AND project_id = ANY($2::text[]) "
                    "AND (observer_stream_id IS NULL OR EXISTS(SELECT 1 FROM shin_server.observer_streams os "
                    "WHERE os.stream_id=observer_stream_id AND os.principal_id=$3)) ORDER BY id ASC LIMIT $4";
            }
            parameters.append(static_cast<std::int64_t>(limit + 1));
            const auto rows = transaction.exec(query, parameters);
            const auto result_limit = static_cast<pqxx::result::size_type>(limit);

            Json items = Json::array();
            const auto returned = std::min(rows.size(), result_limit);
            for (pqxx::result::size_type index = 0; index < returned; ++index) {
                items.push_back(audit_detail::record(rows[index]));
            }
            Json next_cursor = nullptr;
            if (rows.size() > result_limit) {
                next_cursor = rows[result_limit - 1]["id"].as<std::int64_t>();
            }
            transaction.commit();
            return Json{{"items", std::move(items)}, {"next_cursor", std::move(next_cursor)}};
        });
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_database_unavailable();
    } catch (const Json::exception&) {
        throw_database_corrupt();
    }
}

} // namespace shin::server
