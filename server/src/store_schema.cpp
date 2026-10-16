#include "store_schema.h"
#include "store_session.h"
#include "shin/server/store.h"
#include "shin/server/migrations_embedded.h"
#include "shin/server/publication_withdrawals_migration.h"
#include "shin/server/retrieval_journal_epochs_migration.h"
#include "shin/server/private_checkpoints_migration.h"
#include "shin/server/authenticated_receipts_migration.h"
#include "shin/server/publication_history_migration.h"
#include "shin/server/identity_directory_migration.h"
#include "shin/server/external_identities_migration.h"
#include "shin/server/device_sessions_migration.h"
#include "shin/server/workspace_ownership_migration.h"
#include "shin/server/preparation_workspace_migration.h"
#include "shin/server/workspace_publication_history_migration.h"
#include "shin/server/personal_spaces_migration.h"
#include "shin/server/scoped_request_keys_migration.h"
#include "shin/server/workspace_request_keys_migration.h"
#include "shin/server/checkpoint_workspace_slots_migration.h"
#include "shin/server/preparation_scope_keys_migration.h"
#include "shin/server/checkpoint_row_security_migration.h"
#include "shin/server/preparation_row_security_migration.h"
#include "preparation_access.h"
#include "route_access.h"
#include "shin/server/route_history_security_migration.h"
#include "shin/server/observer_ownership_migration.h"
#include "shin/server/journal_attribution_migration.h"
#include "shin/server/identity_directory.h"
#include "shin/server/activation_schema.h"
#include "shin/server/checkpoint_schema.h"
#include "shin/server/workspace_schema.h"
#include "shin/server/observer_ingestion.h"
#include "shin/server/audit.h"
#include "shin/server/recovery.h"
#include "shin/server/identity.h"
#include "shin/server/schema_checks.h"
#include "shin/server/publication_schema.h"
#include "shin/server/retrieval_journal_schema.h"
#include "shin/server/retrieval_journal_migration.h"
#include "shin/server/retrieval_journal_retention_migration.h"

#include <array>
#include <set>
#include <vector>
#include <tuple>

namespace shin::server {
namespace {
struct Migration {
    int version;
    std::string_view checksum;
    std::string_view sql;
};

constexpr std::array<Migration, 35> kMigrations{{
    {1, generated_migrations::initial_checksum, generated_migrations::initial_sql},
    {2, generated_migrations::preparations_checksum, generated_migrations::preparations_sql},
    {3, generated_migrations::retention_checksum, generated_migrations::retention_sql},
    {4, generated_migrations::activation_checksum, generated_migrations::activation_sql},
    {5, generated_migrations::activation_audit_checksum, generated_migrations::activation_audit_sql},
    {6, generated_migrations::workspaces_checksum, generated_migrations::workspaces_sql},
    {7, generated_migrations::observer_checksum, generated_migrations::observer_sql},
    {8, generated_migrations::observer_history_checksum, generated_migrations::observer_history_sql},
    {9, generated_migrations::audit_history_checksum, generated_migrations::audit_history_sql},
    {10, generated_migrations::recovery_checksum, generated_migrations::recovery_sql},
    {11, generated_migrations::observer_registry_checksum, generated_migrations::observer_registry_sql},
    {12, generated_migrations::publication_proposals_checksum, generated_migrations::publication_proposals_sql},
    {13, generated_migrations::publication_withdrawals_checksum, generated_migrations::publication_withdrawals_sql},
    {14, generated_migrations::retrieval_journal_checksum, generated_migrations::retrieval_journal_sql},
    {15, generated_migrations::journal_retention_checksum, generated_migrations::journal_retention_sql},
    {16, generated_migrations::journal_epochs_checksum, generated_migrations::journal_epochs_sql},
    {17, generated_migrations::private_checkpoints_checksum, generated_migrations::private_checkpoints_sql},
    {18, generated_migrations::authenticated_receipts_checksum, generated_migrations::authenticated_receipts_sql},
    {19, generated_migrations::publication_history_checksum, generated_migrations::publication_history_sql},
    {20, generated_migrations::identity_directory_checksum, generated_migrations::identity_directory_sql},
    {21, generated_migrations::workspace_ownership_checksum, generated_migrations::workspace_ownership_sql},
    {22, generated_migrations::preparation_workspace_checksum, generated_migrations::preparation_workspace_sql},
    {23, generated_migrations::workspace_publication_history_checksum, generated_migrations::workspace_publication_history_sql},
    {24, generated_migrations::personal_spaces_checksum, generated_migrations::personal_spaces_sql},
    {25, generated_migrations::scoped_request_keys_checksum, generated_migrations::scoped_request_keys_sql},
    {26, generated_migrations::workspace_request_keys_checksum, generated_migrations::workspace_request_keys_sql},
    {27, generated_migrations::checkpoint_workspace_slots_checksum, generated_migrations::checkpoint_workspace_slots_sql},
    {28, generated_migrations::preparation_scope_keys_checksum, generated_migrations::preparation_scope_keys_sql},
    {29, generated_migrations::checkpoint_row_security_checksum, generated_migrations::checkpoint_row_security_sql},
    {30, generated_migrations::preparation_row_security_checksum, generated_migrations::preparation_row_security_sql},
    {31, generated_migrations::route_history_security_checksum, generated_migrations::route_history_security_sql},
    {32, generated_migrations::journal_attribution_checksum, generated_migrations::journal_attribution_sql},
    {33, generated_migrations::observer_ownership_checksum, generated_migrations::observer_ownership_sql},
    {34, generated_migrations::external_identities_checksum, generated_migrations::external_identities_sql},
    {35, generated_migrations::device_sessions_checksum, generated_migrations::device_sessions_sql},
}};

[[noreturn]] void throw_unsupported_schema() {
    throw ServiceError{503, "schema-unsupported", "The database schema is not supported."};
}

[[noreturn]] void throw_migration_failed() {
    throw ServiceError{503, "migration-failed", "Database migration did not complete."};
}

[[noreturn]] void throw_database_corrupt() {
    throw ServiceError(500, "database-corrupt", "Stored server data is invalid.");
}

void append_base_schema_checks(SchemaChecks& checks) {
    checks.add("SELECT a.attname,a.attgenerated,pg_get_expr(d.adbin,d.adrelid) "
        "FROM pg_attribute a JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
        "WHERE a.attrelid='shin_server.preparations'::regclass AND a.attname IN ('scope_tenant_id','scope_workspace_id')",
        [](const pqxx::result& rows) {
            const std::set<std::pair<std::string, std::string>> expected{
                {"scope_tenant_id", "(workspace_admission #>> '{authorization,ownership,tenant_id}'::text[])"},
                {"scope_workspace_id", "(workspace_admission #>> '{authorization,ownership,workspace_id}'::text[])"}};
            std::set<std::pair<std::string, std::string>> actual;
            for (const auto& row : rows) {
                if (row[1].as<std::string>() != "s") throw_unsupported_schema();
                actual.emplace(row[0].as<std::string>(), row[2].as<std::string>());
            }
            if (actual != expected) throw_unsupported_schema();
        });
    checks.add("SELECT conname,pg_get_constraintdef(oid),convalidated,condeferrable,condeferred,connoinherit "
        "FROM pg_constraint WHERE conrelid='shin_server.preparations'::regclass "
        "AND conname IN ('preparation_scope_identity_check','preparation_scope_project_fk','preparation_scope_workspace_fk')",
        [](const pqxx::result& rows) {
            const std::set<std::pair<std::string, std::string>> expected{
                {"preparation_scope_identity_check", "CHECK (((workspace_admission IS NULL) OR (((scope_tenant_id IS NOT NULL) AND (scope_workspace_id IS NOT NULL) AND ((workspace_admission #>> '{authorization,principal_id}'::text[]) = principal_id) AND ((workspace_admission #>> '{authorization,ownership,project_id}'::text[]) = project_id)) IS TRUE)))"},
                {"preparation_scope_project_fk", "FOREIGN KEY (scope_tenant_id, project_id) REFERENCES shin_server.identity_project_owners(tenant_id, project_id) ON DELETE RESTRICT"},
                {"preparation_scope_workspace_fk", "FOREIGN KEY (project_id, scope_workspace_id) REFERENCES shin_server.identity_workspace_owners(project_id, workspace_id) ON DELETE RESTRICT"}};
            std::set<std::pair<std::string, std::string>> actual;
            for (const auto& row : rows) {
                const bool foreign_key = row[0].as<std::string>() != "preparation_scope_identity_check";
                if (!row[2].as<bool>() || row[3].as<bool>() || row[4].as<bool>() || row[5].as<bool>() != foreign_key)
                    throw_unsupported_schema();
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>());
            }
            if (actual != expected) throw_unsupported_schema();
        });
    checks.add("SELECT pg_get_indexdef(indexrelid),indisvalid AND indisready AND indislive AND indimmediate AND NOT indisunique "
        "FROM pg_index WHERE indexrelid='shin_server.preparations_workspace_cursor_idx'::regclass "
        "AND indrelid='shin_server.preparations'::regclass", [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][1].as<bool>() || rows[0][0].as<std::string>() !=
                "CREATE INDEX preparations_workspace_cursor_idx ON shin_server.preparations USING btree (principal_id, project_id, scope_tenant_id, scope_workspace_id, id)")
                throw_unsupported_schema();
        });
    checks.add("SELECT pg_get_indexdef(i.indexrelid),i.indisvalid AND i.indisready AND i.indislive AND i.indimmediate "
        "FROM pg_index i WHERE i.indrelid='shin_server.preparations'::regclass AND i.indisunique AND NOT i.indisprimary "
        "AND NOT EXISTS(SELECT 1 FROM pg_constraint c WHERE c.conindid=i.indexrelid)",
        [](const pqxx::result& rows) {
            const std::set<std::string> expected{
                "CREATE UNIQUE INDEX preparations_legacy_request_unique ON shin_server.preparations USING btree (principal_id, idempotency_key) WHERE (workspace_admission IS NULL)",
                "CREATE UNIQUE INDEX preparations_workspace_request_unique ON shin_server.preparations USING btree (principal_id, project_id, scope_tenant_id, scope_workspace_id, idempotency_key) WHERE (workspace_admission IS NOT NULL)"};
            std::set<std::string> actual;
            for (const auto& row : rows) {
                if (!row[1].as<bool>()) throw_unsupported_schema();
                actual.insert(row[0].as<std::string>());
            }
            if (actual != expected) throw_unsupported_schema();
        });
    checks.add("SELECT count(*)=3 FROM pg_constraint WHERE conrelid='shin_server.preparations'::regclass AND contype='u'",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>()) throw_unsupported_schema();
        });
    checks.add("SELECT pg_get_constraintdef(oid),convalidated,connoinherit FROM pg_constraint "
        "WHERE conrelid='shin_server.preparations'::regclass AND conname='preparation_workspace_admission_check' AND contype='c'",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][1].as<bool>() || rows[0][2].as<bool>() || rows[0][0].as<std::string>() !=
                "CHECK (((workspace_admission IS NULL) OR ((jsonb_typeof(workspace_admission) = 'object'::text) AND (octet_length((workspace_admission)::text) <= 32768))))")
                throw_unsupported_schema();
        });
    checks.add("SELECT c.relname AS table_name, a.attname AS column_name, "
        "pg_catalog.format_type(a.atttypid, a.atttypmod) AS type_name, a.attnotnull, a.attidentity "
        "FROM pg_catalog.pg_attribute AS a "
        "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'shin_server' "
        "AND c.relkind = 'r' "
        "AND c.relname IN ('schema_migrations', 'projects', 'audit_events', 'idempotency_requests', 'preparations', "
        "'preparation_retention') "
        "AND a.attnum > 0 AND NOT a.attisdropped", [](const pqxx::result& actual_rows) {
        using Column = std::tuple<std::string, std::string, std::string, bool, std::string>;
        static const std::set<Column> expected{
            {"audit_events", "activation_route_id", "text", false, ""},
            {"audit_events", "activation_previous_revision", "bigint", false, ""},
            {"audit_events", "action", "text", true, ""},
            {"audit_events", "created_at", "timestamp with time zone", true, ""},
            {"audit_events", "id", "bigint", true, "a"},
            {"audit_events", "principal_id", "text", true, ""},
            {"audit_events", "preparation_id", "bigint", false, ""},
            {"audit_events", "project_id", "text", true, ""},
            {"audit_events", "workspace_id", "text", false, ""},
            {"audit_events", "observer_stream_id", "text", false, ""},
            {"audit_events", "observer_sequence", "bigint", false, ""},
            {"audit_events", "publication_proposal_id", "bigint", false, ""},
            {"idempotency_requests", "created_at", "timestamp with time zone", true, ""},
            {"idempotency_requests", "key", "text", true, ""},
            {"idempotency_requests", "principal_id", "text", true, ""},
            {"idempotency_requests", "project", "jsonb", true, ""},
            {"idempotency_requests", "request_hash", "text", true, ""},
            {"preparation_retention", "completed_by", "text", false, ""},
            {"preparation_retention", "inventory", "jsonb", true, ""},
            {"preparation_retention", "plan_id", "text", true, ""},
            {"preparation_retention", "preparation_id", "bigint", true, ""},
            {"preparation_retention", "released_at", "timestamp with time zone", false, ""},
            {"preparation_retention", "requested_at", "timestamp with time zone", true, ""},
            {"preparation_retention", "requested_by", "text", true, ""},
            {"preparation_retention", "state", "text", true, ""},
            {"preparations", "artifact_bytes", "bigint", false, ""},
            {"preparations", "artifact_key", "text", false, ""},
            {"preparations", "artifact_namespace", "text", false, ""},
            {"preparations", "artifact_snapshot_id", "text", false, ""},
            {"preparations", "cancel_requested", "boolean", true, ""},
            {"preparations", "created_at", "timestamp with time zone", true, ""},
            {"preparations", "expected_snapshot_id", "text", true, ""},
            {"preparations", "expected_route_revision", "bigint", true, ""},
            {"preparations", "id", "bigint", true, "a"},
            {"preparations", "idempotency_key", "text", true, ""},
            {"preparations", "lease_until", "timestamp with time zone", false, ""},
            {"preparations", "principal_id", "text", true, ""},
            {"preparations", "project_id", "text", true, ""},
            {"preparations", "reason_code", "text", false, ""},
            {"preparations", "request_hash", "text", true, ""},
            {"preparations", "reserved_artifact_bytes", "bigint", true, ""},
            {"preparations", "route_fingerprint", "text", true, ""},
            {"preparations", "route_id", "text", true, ""},
            {"preparations", "state", "text", true, ""},
            {"preparations", "updated_at", "timestamp with time zone", true, ""},
            {"preparations", "worker_id", "text", false, ""},
            {"preparations", "workspace_admission", "jsonb", false, ""},
            {"preparations", "scope_tenant_id", "text", false, ""},
            {"preparations", "scope_workspace_id", "text", false, ""},
            {"projects", "created_at", "timestamp with time zone", true, ""},
            {"projects", "display_name", "text", true, ""},
            {"projects", "id", "text", true, ""},
            {"projects", "revision", "bigint", true, ""},
            {"schema_migrations", "applied_at", "timestamp with time zone", true, ""},
            {"schema_migrations", "checksum", "text", true, ""},
            {"schema_migrations", "version", "integer", true, ""},
        };

        std::set<Column> actual;
        for (const auto& row : actual_rows) {
            actual.emplace(row["table_name"].as<std::string>(), row["column_name"].as<std::string>(),
                           row["type_name"].as<std::string>(), row["attnotnull"].as<bool>(),
                           row["attidentity"].as<std::string>());
        }
        if (actual != expected) {
            throw_unsupported_schema();
        }
    });
    checks.add("SELECT to_regclass('shin_server.audit_events_id_seq') IS NOT NULL AS audit_sequence_exists, "
        "to_regclass('shin_server.preparations_id_seq') IS NOT NULL AS preparation_sequence_exists", [](const pqxx::result& sequences) {
        if (sequences.size() != 1 || !sequences[0]["audit_sequence_exists"].as<bool>() ||
            !sequences[0]["preparation_sequence_exists"].as<bool>()) {
            throw_unsupported_schema();
        }
    });
    checks.add("SELECT "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.schema_migrations'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1]::smallint[]) AS migration_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.projects'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1]::smallint[]) AS project_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.audit_events'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1]::smallint[]) AS audit_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.audit_events'::regclass "
        "AND c.contype = 'f' AND c.conkey = ARRAY[4]::smallint[] "
        "AND c.confrelid = 'shin_server.projects'::regclass "
        "AND c.confkey = ARRAY[1]::smallint[] AND c.confdeltype = 'r') AS audit_project_foreign_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.idempotency_requests'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1, 2]::smallint[]) AS idempotency_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparations'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1]::smallint[]) AS preparation_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparations'::regclass "
        "AND c.contype = 'f' AND c.conkey = ARRAY[2]::smallint[] "
        "AND c.confrelid = 'shin_server.projects'::regclass "
        "AND c.confkey = ARRAY[1]::smallint[] AND c.confdeltype = 'r') AS preparation_project_foreign_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparations'::regclass "
        "AND c.contype = 'u' AND c.conkey = ARRAY[20]::smallint[]) AS preparation_artifact_key_unique, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparation_retention'::regclass "
        "AND c.contype = 'p' AND c.conkey = ARRAY[1]::smallint[]) AS retention_primary_key, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparation_retention'::regclass "
        "AND c.contype = 'f' AND c.conkey = ARRAY[1]::smallint[] "
        "AND c.confrelid = 'shin_server.preparations'::regclass "
        "AND c.confkey = ARRAY[1]::smallint[] AND c.confdeltype = 'r') AS retention_preparation_foreign_key, "
        "(SELECT count(*) FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.projects'::regclass AND c.contype = 'c') = 3 AS project_checks, "
        "(SELECT count(*) FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.audit_events'::regclass AND c.contype = 'c') = 4 AS audit_checks, "
        "(SELECT count(*) FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.idempotency_requests'::regclass AND c.contype = 'c') = 4 AS idempotency_checks, "
        "(SELECT count(*) FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparations'::regclass AND c.contype = 'c') = 17 AS preparation_checks, "
        "(SELECT count(*) FROM pg_catalog.pg_constraint AS c "
        "WHERE c.conrelid = 'shin_server.preparation_retention'::regclass AND c.contype = 'c') = 6 AS retention_checks, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_index AS i "
        "WHERE i.indexrelid = 'shin_server.audit_events_project_id_id_idx'::regclass "
        "AND i.indrelid = 'shin_server.audit_events'::regclass "
        "AND i.indkey = '4 1'::int2vector) AS audit_project_cursor_index, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_index AS i "
        "WHERE i.indexrelid = 'shin_server.preparations_project_id_id_idx'::regclass "
        "AND i.indrelid = 'shin_server.preparations'::regclass "
        "AND i.indkey = '2 1'::int2vector) AS preparation_project_cursor_index, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_index AS i "
        "WHERE i.indexrelid = 'shin_server.preparations_queue_idx'::regclass "
        "AND i.indrelid = 'shin_server.preparations'::regclass "
        "AND i.indkey = '9 1'::int2vector) AS preparation_queue_index, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_index AS i "
        "WHERE i.indexrelid = 'shin_server.preparations_lease_idx'::regclass "
        "AND i.indrelid = 'shin_server.preparations'::regclass "
        "AND i.indkey = '9 12 1'::int2vector) AS preparation_lease_index, "
        "EXISTS (SELECT 1 FROM pg_catalog.pg_index AS i "
        "WHERE i.indexrelid = 'shin_server.preparation_retention_pkey'::regclass "
        "AND i.indrelid = 'shin_server.preparation_retention'::regclass "
        "AND i.indkey = '1'::int2vector) AS retention_preparation_index", [](const pqxx::result& constraints) {
        if (constraints.size() != 1) {
            throw_unsupported_schema();
        }
        const auto& layout = constraints[0];
        if (!layout["migration_primary_key"].as<bool>() || !layout["project_primary_key"].as<bool>() ||
            !layout["audit_primary_key"].as<bool>() ||
            !layout["audit_project_foreign_key"].as<bool>() || !layout["idempotency_primary_key"].as<bool>() ||
            !layout["preparation_primary_key"].as<bool>() || !layout["preparation_project_foreign_key"].as<bool>() ||
            !layout["preparation_artifact_key_unique"].as<bool>() ||
            !layout["retention_primary_key"].as<bool>() || !layout["retention_preparation_foreign_key"].as<bool>() ||
            !layout["project_checks"].as<bool>() || !layout["audit_checks"].as<bool>() ||
            !layout["idempotency_checks"].as<bool>() || !layout["preparation_checks"].as<bool>() ||
            !layout["retention_checks"].as<bool>() ||
            !layout["audit_project_cursor_index"].as<bool>() || !layout["preparation_project_cursor_index"].as<bool>() ||
            !layout["preparation_queue_index"].as<bool>() || !layout["preparation_lease_index"].as<bool>() ||
            !layout["retention_preparation_index"].as<bool>()) {
            throw_unsupported_schema();
        }
    });
}

void verify_schema_layout(pqxx::transaction_base& transaction) {
    static const SchemaChecks checks = [] {
        SchemaChecks plan;
        append_base_schema_checks(plan);
        append_activation_schema_checks(plan);
        append_workspace_schema_checks(plan);
        append_observer_schema_checks(plan);
        append_audit_schema_checks(plan);
        append_recovery_schema_checks(plan);
        append_server_identity_schema_checks(plan);
        append_publication_schema_checks(plan);
        append_retrieval_journal_schema_checks(plan);
        append_checkpoint_schema_checks(plan);
        append_identity_schema_checks(plan);
        append_external_identity_schema_checks(plan);
        append_device_schema_checks(plan);
        append_ownership_schema_checks(plan);
        append_preparation_access_checks(plan);
        append_route_history_checks(plan);
        return plan;
    }();
    checks.run(transaction);
}

void apply_runtime_grants(pqxx::transaction_base& transaction, std::string_view runtime_role) {
    const auto role = transaction.quote_name(runtime_role);
    transaction.exec("REVOKE ALL ON SCHEMA shin_server FROM PUBLIC");
    transaction.exec("REVOKE ALL ON ALL TABLES IN SCHEMA shin_server FROM PUBLIC");
    transaction.exec("REVOKE ALL ON ALL SEQUENCES IN SCHEMA shin_server FROM PUBLIC");
    transaction.exec("GRANT EXECUTE ON FUNCTION shin_server.current_workspace_allowed() TO " + role);
    transaction.exec("REVOKE ALL ON SCHEMA shin_server FROM " + role);
    transaction.exec("REVOKE ALL ON ALL TABLES IN SCHEMA shin_server FROM " + role);
    transaction.exec("REVOKE ALL ON ALL SEQUENCES IN SCHEMA shin_server FROM " + role);
    transaction.exec("GRANT USAGE ON SCHEMA shin_server TO " + role);
    transaction.exec(
        "GRANT SELECT, INSERT ON TABLE shin_server.projects, shin_server.audit_events, "
        "shin_server.idempotency_requests TO " +
        role);
    transaction.exec("GRANT SELECT, INSERT, UPDATE ON TABLE shin_server.preparations, shin_server.preparation_retention, shin_server.active_routes TO " + role);
    transaction.exec("GRANT SELECT, INSERT ON TABLE shin_server.activation_receipts TO " + role);
    transaction.exec("GRANT SELECT, INSERT ON TABLE shin_server.workspaces, shin_server.workspace_requests TO " + role);
    transaction.exec("GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE shin_server.observer_streams TO " + role);
    transaction.exec("GRANT SELECT, INSERT ON TABLE shin_server.observer_tombstones TO " + role);
    transaction.exec("GRANT SELECT ON TABLE shin_server.server_identity TO " + role);
    transaction.exec("GRANT SELECT, UPDATE ON TABLE shin_server.observer_usage TO " + role);
    transaction.exec("GRANT SELECT, INSERT, DELETE ON TABLE shin_server.observer_submissions TO " + role);
    transaction.exec("GRANT SELECT, UPDATE ON TABLE shin_server.audit_history TO " + role);
    transaction.exec("GRANT DELETE ON TABLE shin_server.audit_events TO " + role);
    transaction.exec("GRANT SELECT ON TABLE shin_server.server_recovery TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.audit_events_id_seq, shin_server.preparations_id_seq TO " + role);
    transaction.exec("GRANT SELECT ON TABLE shin_server.schema_migrations TO " + role);
    transaction.exec("GRANT SELECT, INSERT, DELETE ON TABLE shin_server.publication_proposals, shin_server.publication_resolutions, shin_server.publication_withdrawals TO " + role);
    transaction.exec("GRANT SELECT, UPDATE ON TABLE shin_server.publication_history TO " + role);
    transaction.exec("GRANT SELECT, INSERT ON TABLE shin_server.workspace_publication_history TO " + role);
    transaction.exec("GRANT UPDATE(history_epoch,retired_proposals,last_retention) "
                     "ON TABLE shin_server.workspace_publication_history TO " + role);
    transaction.exec("GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE shin_server.publication_heads TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.publication_proposals_id_seq, shin_server.publication_withdrawals_id_seq TO " + role);
    transaction.exec("GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE shin_server.retrieval_journal_producers TO " + role);
    transaction.exec("GRANT SELECT, INSERT, DELETE ON TABLE shin_server.retrieval_journal_attempts TO " + role);
    transaction.exec("GRANT SELECT, UPDATE ON TABLE shin_server.retrieval_journal_lifecycle TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.retrieval_journal_attempts_id_seq TO " + role);
    transaction.exec("GRANT SELECT, INSERT, UPDATE, DELETE ON TABLE shin_server.checkpoint_slots TO " + role);
    transaction.exec("GRANT SELECT, UPDATE ON TABLE shin_server.checkpoint_lifecycle TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.checkpoint_revisions TO " + role);
    transaction.exec("GRANT SELECT,INSERT,UPDATE ON shin_server.identity_accounts,shin_server.identity_tenants,"
                     "shin_server.identity_memberships TO " + role);
    transaction.exec("GRANT SELECT,INSERT ON shin_server.identity_audit TO " + role);
    transaction.exec("GRANT SELECT,INSERT ON shin_server.identity_external_bindings TO " + role);
    transaction.exec("GRANT UPDATE(enabled,revision) ON shin_server.identity_external_bindings TO " + role);
    transaction.exec("GRANT SELECT,INSERT ON shin_server.identity_devices TO " + role);
    transaction.exec("GRANT UPDATE(revoked,revision) ON shin_server.identity_devices TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.identity_audit_id_seq TO " + role);
    transaction.exec("GRANT SELECT,INSERT ON shin_server.identity_project_owners,shin_server.identity_workspace_owners,"
                     "shin_server.identity_ownership_audit TO " + role);
    transaction.exec("GRANT USAGE ON SEQUENCE shin_server.identity_ownership_audit_id_seq TO " + role);
}

void preflight_migration_schema(pqxx::transaction_base& transaction) {
    const auto state = transaction.exec(
        "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname = 'shin_server') AS schema_exists, "
        "to_regclass('shin_server.schema_migrations') IS NOT NULL AS history_exists");
    if (state.size() != 1) {
        throw_unsupported_schema();
    }

    const bool schema_exists = state[0]["schema_exists"].as<bool>();
    const bool history_exists = state[0]["history_exists"].as<bool>();
    if (!schema_exists || history_exists) {
        return;
    }

    const auto relations = transaction.exec(
        "SELECT EXISTS ("
        "SELECT 1 FROM pg_catalog.pg_class AS c "
        "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
        "WHERE n.nspname = 'shin_server') AS has_relations");
    if (relations.size() != 1 || relations[0]["has_relations"].as<bool>()) {
        throw_unsupported_schema();
    }
}

std::size_t validated_applied_migrations(pqxx::transaction_base& transaction) {
    const auto applied = transaction.exec(
        "SELECT version, checksum FROM shin_server.schema_migrations ORDER BY version ASC");
    const auto applied_count = applied.size();
    if (static_cast<std::size_t>(applied_count) > kMigrations.size()) {
        throw_unsupported_schema();
    }

    for (pqxx::result::size_type index = 0; index < applied_count; ++index) {
        const auto version = applied[index]["version"].as<int>();
        const auto checksum = applied[index]["checksum"].as<std::string>();
        const auto& expected = kMigrations[static_cast<std::size_t>(index)];
        if (version != expected.version || checksum != expected.checksum) {
            throw_unsupported_schema();
        }
    }
    return static_cast<std::size_t>(applied_count);
}

void apply_migrations(pqxx::transaction_base& transaction) {
    preflight_migration_schema(transaction);

    bool history_exists = transaction.exec(
        "SELECT to_regclass('shin_server.schema_migrations') IS NOT NULL AS history_exists")[0]
                              ["history_exists"]
                                  .as<bool>();
    std::size_t applied_count = 0;
    if (history_exists) {
        applied_count = validated_applied_migrations(transaction);
        if (applied_count == 0) {
            const auto relations = transaction.exec(
                "SELECT EXISTS ("
                "SELECT 1 FROM pg_catalog.pg_class AS c "
                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                "WHERE n.nspname = 'shin_server' AND c.relname <> 'schema_migrations') AS has_relations");
            if (relations.size() != 1 || relations[0]["has_relations"].as<bool>()) {
                throw_unsupported_schema();
            }
        }
    }

    for (std::size_t index = applied_count; index < kMigrations.size(); ++index) {
        const auto& migration = kMigrations[index];
        transaction.exec(migration.sql);

        pqxx::params parameters{transaction};
        parameters.append(migration.version);
        parameters.append(migration.checksum);
        transaction.exec(
            "INSERT INTO shin_server.schema_migrations (version, checksum) VALUES ($1, $2)", parameters);
    }

    verify_schema_layout(transaction);
}

} // namespace

void verify_runtime_schema(pqxx::transaction_base& transaction) {
    const auto history = transaction.exec(
        "SELECT to_regclass('shin_server.schema_migrations') IS NOT NULL AS history_exists");
    if (history.size() != 1 || !history[0]["history_exists"].as<bool>() ||
        validated_applied_migrations(transaction) != kMigrations.size()) {
        throw_unsupported_schema();
    }
    verify_schema_layout(transaction);
}

void Store::migrate(const StoreConfig& config) {
    validate_store_config(config);
    try {
        auto connection = open_store_connection(config);
        verify_database_name(connection, config);

        pqxx::work transaction{connection};
        if (!transaction.exec("SELECT pg_try_advisory_xact_lock(hashtext('shin_server.lifecycle.v1'))")[0][0].as<bool>())
            throw ServiceError(409, "server-recovery-busy", "Stop Server database sessions before maintenance.");
        transaction.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.migrations.v1'))");
        const auto missing_history = transaction.exec("SELECT to_regclass('shin_server.schema_migrations') IS NULL")[0][0].as<bool>();
        const auto fresh = missing_history || transaction.exec("SELECT count(*)=0 FROM shin_server.schema_migrations")[0][0].as<bool>();
        transaction.exec("SELECT set_config('shin_server.migration_source_host_id', $1, true)",
                         pqxx::params{transaction, config.source_host_id});
        apply_migrations(transaction);
        if (fresh) transaction.exec("UPDATE shin_server.server_identity SET server_id=$1 WHERE singleton",
                                    pqxx::params{transaction, config.expected_server_id});
        apply_runtime_grants(transaction, config.runtime_role);
        verify_checkpoint_runtime_grants(transaction, config.runtime_role);
        transaction.commit();
    } catch (const ServiceError&) {
        throw;
    } catch (const pqxx::failure&) {
        throw_migration_failed();
    }
}

Json Store::recover(const StoreConfig& config, const Json& input, bool apply, IdentityMaintenance mode) {
    validate_store_config(config);
    try {
        auto connection = open_store_connection(config);
        verify_database_name(connection, config);
        pqxx::work tx{connection};
        if (!tx.exec("SELECT pg_try_advisory_xact_lock(hashtext('shin_server.lifecycle.v1'))")[0][0].as<bool>())
            throw ServiceError(409, "server-recovery-busy", "Stop Server database sessions before maintenance.");
        std::vector<std::string> owned_tables{"server_recovery"};
        if (mode == IdentityMaintenance::observer_retirement)
            owned_tables.insert(owned_tables.end(), {"server_identity", "observer_usage", "observer_streams", "observer_tombstones"});
        const auto owner = tx.exec("SELECT count(*)=$2 AND bool_and(current_user=pg_get_userbyid(n.nspowner) AND c.relowner=n.nspowner) "
            "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
            "WHERE n.nspname='shin_server' AND c.relname=ANY($1::text[])",
            pqxx::params{tx, owned_tables, owned_tables.size()});
        if (owner.size() != 1 || !owner[0][0].as<bool>())
            throw ServiceError(403, "recovery-role-denied", "Recovery requires the schema owner, not the runtime role.");
        tx.exec("SELECT set_config('shin.observer_ops','1',true),set_config('shin.observer_quota','1',true)");
        verify_runtime_schema(tx);
        auto result = recovery_operation(tx, config.expected_server_id, input, apply, mode);
        tx.commit();
        return result;
    } catch (const ServiceError&) { throw; }
    catch (const pqxx::failure&) { throw_database_unavailable(); }
    catch (const Json::exception&) { throw_database_corrupt(); }
}

} // namespace shin::server
