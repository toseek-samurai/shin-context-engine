#pragma once

#include "model.h"
#include "schema_checks.h"

namespace pqxx { class transaction_base; }

namespace shin::server {
enum class IdentityOperation { tenant_read, membership_write };
bool identity_role_allows(std::string_view role, IdentityOperation operation);
void identity_scope(pqxx::transaction_base& tx, const Principal& actor);
void authorize_identity_tenant(pqxx::transaction_base& tx, const Principal& actor,
                               const std::string& tenant, IdentityOperation operation);
void append_identity_schema_checks(SchemaChecks& checks);
void append_external_identity_schema_checks(SchemaChecks& checks);
void append_device_schema_checks(SchemaChecks& checks);
void append_ownership_schema_checks(SchemaChecks& checks);
// Metadata authorization only; never use this result as a cached content grant.
Json authorize_identity_workspace(pqxx::transaction_base& tx, const Principal& actor,
                                  const std::string& tenant, const std::string& project,
                                  const std::string& workspace);
// Live internal admission snapshot; callers hold the directory shared lock.
Json scoped_workspace_snapshot(pqxx::transaction_base& tx, const Principal& actor,
                               const std::string& project, const WorkspaceReadRequest& scope);
Json workspace_owner_admission(pqxx::transaction_base& tx, const Principal& actor,
                               const std::string& project, const WorkspaceReadRequest& scope);
void authorize_workspace_write(pqxx::transaction_base& tx, const Principal& actor,
                               const std::string& project, const WorkspaceWriteScope& scope);
void require_scoped_candidate(pqxx::transaction_base& tx, const Principal& actor,
                              std::uint64_t id, const WorkspaceWriteScope& scope);
// Current authority may clean an obsolete job, but never inspect it for reuse.
void authorize_preparation_cleanup(pqxx::transaction_base& tx, const Principal& actor,
                                   std::uint64_t id, const WorkspaceWriteScope& scope);
} // namespace shin::server
