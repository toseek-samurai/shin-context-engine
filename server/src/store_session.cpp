#include "store_session.h"
#include "store_schema.h"
#include "shin/server/checkpoint_schema.h"
#include "shin/server/config.h"
#include "shin/server/identity.h"

#include <map>
#include <string_view>

namespace shin::server {
namespace {
constexpr std::size_t kMaxPoolSize = 16;
constexpr std::string_view kRuntimeRoleError = "Database runtime identity is not accepted.";
bool has_nul(std::string_view value) { return value.find('\0') != std::string_view::npos; }
[[noreturn]] void throw_runtime_role_error() {
    throw ServiceError{503, "database-configuration-invalid", std::string{kRuntimeRoleError}};
}

} // namespace

[[noreturn]] void throw_database_unavailable() {
    throw ServiceError{503, "database-unavailable", "The server database is unavailable."};
}

void validate_store_config(const StoreConfig& config) {
    if (config.connection_string.empty() || config.connection_string.size() > 16384 ||
        has_nul(config.connection_string) || config.expected_database.empty() ||
        config.expected_database.size() > 63 || has_nul(config.expected_database) ||
        config.runtime_role.empty() || config.runtime_role.size() > 63 ||
        has_nul(config.runtime_role) || config.pool_size == 0 || config.pool_size > kMaxPoolSize ||
        !valid_identifier(config.source_host_id) || config.expected_server_id.size() != 32 ||
        config.expected_server_id.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw_runtime_role_error();
    }
}

pqxx::connection open_store_connection(const StoreConfig& config) {
    const std::map<std::string, std::string> forced_parameters{
        {"connect_timeout", "2"},
        {"options", "-c statement_timeout=2000ms -c lock_timeout=500ms"},
    };
    pqxx::connection connection{config.connection_string, forced_parameters};
    connection.set_notice_handler([](pqxx::zview) {});
    return connection;
}

void verify_database_name(pqxx::connection& connection, const StoreConfig& config) {
    pqxx::read_transaction transaction{connection};
    const auto result = transaction.exec("SELECT current_database() AS database_name");
    if (result.size() != 1 || result[0]["database_name"].as<std::string>() != config.expected_database) {
        throw_runtime_role_error();
    }
    transaction.commit();
}

void verify_runtime_connection(pqxx::connection& connection, const StoreConfig& config) {
    pqxx::read_transaction transaction{connection};
    const auto result = transaction.exec(
        "SELECT current_database() AS database_name, current_user AS role_name, "
        "(SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user) AS is_superuser, "
        "(SELECT rolcreatedb FROM pg_catalog.pg_roles WHERE rolname = current_user) AS can_create_database, "
        "(SELECT rolcreaterole FROM pg_catalog.pg_roles WHERE rolname = current_user) AS can_create_role, "
        "(SELECT rolreplication FROM pg_catalog.pg_roles WHERE rolname = current_user) AS can_replicate, "
        "(SELECT rolbypassrls FROM pg_catalog.pg_roles WHERE rolname = current_user) AS can_bypass_rls, "
        "has_schema_privilege(current_user, 'shin_server', 'CREATE') AS can_create_schema, "
        // This verification transaction ends before the application lease starts.
        "set_config('shin.audit_ops','1',true),set_config('shin.journal_ops','1',true),"
        "set_config('shin.observer_ops','1',true),set_config('shin.observer_quota','1',true)");
    if (result.size() != 1 || result[0]["database_name"].as<std::string>() != config.expected_database ||
        result[0]["role_name"].as<std::string>() != config.runtime_role ||
        result[0]["is_superuser"].as<bool>() || result[0]["can_create_database"].as<bool>() ||
        result[0]["can_create_role"].as<bool>() || result[0]["can_replicate"].as<bool>() ||
        result[0]["can_bypass_rls"].as<bool>() || result[0]["can_create_schema"].as<bool>()) {
        throw_runtime_role_error();
    }
    verify_runtime_schema(transaction);
    verify_checkpoint_runtime_grants(transaction, config.runtime_role);
    if (read_server_identity(transaction) != config.expected_server_id)
        throw ServiceError(503, "server-identity-mismatch", "Database identity does not match this deployment. Explicit recovery review is required.");
    transaction.commit();
}

} // namespace shin::server
