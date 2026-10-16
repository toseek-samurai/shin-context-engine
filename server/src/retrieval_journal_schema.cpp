#include "shin/server/retrieval_journal_schema.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>

#include <initializer_list>
#include <set>
#include <string>
#include <tuple>

namespace shin::server {
namespace {
using Column = std::tuple<std::string, int, std::string, std::string, bool, std::string, std::string>;
using Constraint = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string>;
using Index = std::tuple<std::string, std::string, std::string, bool, bool, std::string>;

struct ColumnSpec {
    const char* name;
    const char* type;
    bool required;
    const char* identity = "";
    const char* default_expression = "";
};

void add_columns(std::set<Column>& expected, const char* table, std::initializer_list<ColumnSpec> columns) {
    int position = 1;
    for (const auto& column : columns)
        expected.emplace(table, position++, column.name, column.type, column.required, column.identity, column.default_expression);
}

void add_check(std::set<Constraint>& expected, const char* table, const char* name,
               const char* columns, const char* expression) {
    expected.emplace(table, name, "c", columns, "", "", expression);
}

void unsupported(const char* part) {
    throw ServiceError(503, "schema-unsupported", std::string("Retrieval journal schema ") + part + " are not supported.");
}
} // namespace

void append_retrieval_journal_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attnum,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,a.attidentity, "
        "COALESCE(pg_get_expr(d.adbin,d.adrelid),'') "
        "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
        "WHERE n.nspname='shin_server' AND c.relkind='r' "
        "AND c.relname IN ('retrieval_journal_producers','retrieval_journal_attempts','retrieval_journal_lifecycle') "
        "AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            static const std::set<Column> expected = [] {
                std::set<Column> value;
                add_columns(value, "retrieval_journal_producers", {
                    {"instance_id", "text", true}, {"source_host_id", "text", true},
                    {"server_version", "text", true}, {"core_version", "text", true},
                    {"observed_through", "bigint", true, "", "0"}, {"captured_count", "bigint", true, "", "0"},
                    {"missing_count", "bigint", true, "", "0"},
                    {"created_at", "timestamp with time zone", true, "", "clock_timestamp()"},
                    {"updated_at", "timestamp with time zone", true, "", "clock_timestamp()"},
                    {"retired_through", "bigint", true, "", "0"}, {"retired_count", "bigint", true, "", "0"},
                    {"retired_missing_count", "bigint", true, "", "0"}, {"last_retention", "jsonb", false},
                    {"producer_epoch", "bigint", true},
                });
                add_columns(value, "retrieval_journal_attempts", {
                    {"id", "bigint", true, "a"}, {"instance_id", "text", true}, {"sequence", "bigint", true},
                    {"record_hash", "text", true}, {"project_id", "text", true}, {"operation", "text", true},
                    {"selector_kind", "text", true}, {"selector_id", "text", true},
                    {"observed_at_unix_ms", "bigint", true}, {"status", "integer", true},
                    {"reason_code", "text", false}, {"last_phase", "text", true}, {"elapsed_ns", "bigint", true},
                    {"result_bytes", "bigint", false}, {"validation_ns", "bigint", true},
                    {"selection_ns", "bigint", false}, {"admission_ns", "bigint", false},
                    {"execution_ns", "bigint", false}, {"finalization_ns", "bigint", false},
                    {"route_id", "text", false}, {"workspace_id", "text", false}, {"source_host_id", "text", false},
                    {"snapshot_id", "text", false}, {"preparation_id", "bigint", false}, {"revision", "bigint", false},
                    {"profile_id", "text", false}, {"profile_fingerprint", "text", false},
                    {"route_fingerprint", "text", false}, {"retrieval_mode", "text", false},
                    {"returned_records", "bigint", false}, {"candidate_rows", "bigint", false},
                    {"covered_groups", "bigint", false}, {"total_groups", "bigint", false},
                    {"pack_complete", "boolean", false}, {"pack_coverage_complete", "boolean", false},
                    {"required_requirements", "bigint", false}, {"satisfied_required_requirements", "bigint", false},
                    {"query_capture", "text", true}, {"request_fingerprint", "text", false}, {"request", "jsonb", false},
                    {"received_at", "timestamp with time zone", true, "", "clock_timestamp()"},
                    {"producer_epoch", "bigint", true},
                    {"scope_principal_id", "text", false}, {"scope_tenant_id", "text", false}, {"scope_workspace_id", "text", false},
                });
                add_columns(value, "retrieval_journal_lifecycle", {
                    {"singleton", "boolean", true, "", "true"},
                    {"allocated_through", "bigint", true, "", "0"},
                    {"reclaimed_through", "bigint", true, "", "0"},
                    {"reclaimed_producer_count", "bigint", true, "", "0"},
                    {"reclaimed_captured_count", "bigint", true, "", "0"},
                    {"reclaimed_missing_count", "bigint", true, "", "0"},
                    {"last_reclamation", "jsonb", false},
                });
                return value;
            }();
            std::set<Column> actual;
            for (const auto& row : rows) {
                actual.emplace(row[0].as<std::string>(), row[1].as<int>(), row[2].as<std::string>(), row[3].as<std::string>(),
                               row[4].as<bool>(), row[5].as<std::string>(), row[6].as<std::string>());
            }
            if (actual != expected) unsupported("columns");
        });
    checks.add(
        "SELECT t.relname,c.conname,c.contype, "
        "array_to_string(ARRAY(SELECT a.attname FROM unnest(c.conkey) WITH ORDINALITY k(num,pos) "
        "JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=k.num "
        "ORDER BY CASE WHEN c.contype='c' THEN k.num ELSE k.pos END),','), "
        "COALESCE(rn.nspname||'.'||r.relname,''), "
        "array_to_string(ARRAY(SELECT a.attname FROM unnest(c.confkey) WITH ORDINALITY k(num,pos) "
        "JOIN pg_attribute a ON a.attrelid=c.confrelid AND a.attnum=k.num ORDER BY k.pos),','), "
        "COALESCE(pg_get_expr(c.conbin,c.conrelid),''),c.convalidated, "
        "NOT c.condeferrable AND NOT c.condeferred AND (c.contype<>'c' OR NOT c.connoinherit), "
        "(c.contype<>'f' OR (c.confdeltype='r' AND c.confupdtype='a' AND c.confmatchtype='s')) "
        "FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid JOIN pg_namespace n ON n.oid=t.relnamespace "
        "LEFT JOIN pg_class r ON r.oid=c.confrelid LEFT JOIN pg_namespace rn ON rn.oid=r.relnamespace "
        "WHERE n.nspname='shin_server' AND c.contype IN ('p','u','f','c') "
        "AND t.relname IN ('retrieval_journal_producers','retrieval_journal_attempts','retrieval_journal_lifecycle')",
        [](const pqxx::result& rows) {
            static const std::set<Constraint> expected = [] {
                std::set<Constraint> value{
                    {"retrieval_journal_producers","retrieval_journal_producers_pkey","p","instance_id","","",""},
                    {"retrieval_journal_producers","retrieval_journal_producers_epoch_unique","u","producer_epoch","","",""},
                    {"retrieval_journal_producers","retrieval_journal_producers_instance_epoch_unique","u","instance_id,producer_epoch","","",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_pkey","p","id","","",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_instance_sequence_unique","u","instance_id,sequence","","",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_instance_fk","f","instance_id,producer_epoch",
                     "shin_server.retrieval_journal_producers","instance_id,producer_epoch",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_project_fk","f","project_id","shin_server.projects","id",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_principal_fk","f","scope_principal_id","shin_server.identity_accounts","principal_id",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_tenant_fk","f","scope_tenant_id,project_id","shin_server.identity_project_owners","tenant_id,project_id",""},
                    {"retrieval_journal_attempts","retrieval_journal_attempts_workspace_fk","f","project_id,scope_workspace_id","shin_server.identity_workspace_owners","project_id,workspace_id",""},
                    {"retrieval_journal_lifecycle","retrieval_journal_lifecycle_pkey","p","singleton","","",""},
                };
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_instance_check", "instance_id",
                          "(instance_id ~ '^[0-9a-f]{64}$'::text)");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_source_host_check", "source_host_id",
                          "(source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_server_version_check", "server_version",
                          "((octet_length(server_version) >= 1) AND (octet_length(server_version) <= 128))");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_core_version_check", "core_version",
                          "((octet_length(core_version) >= 1) AND (octet_length(core_version) <= 128))");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_progress_check",
                          "observed_through,captured_count,missing_count",
                          "((observed_through >= 0) AND (captured_count >= 0) AND (missing_count >= 0) AND ((captured_count + missing_count) = observed_through))");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_retirement_check",
                          "observed_through,captured_count,missing_count,retired_through,retired_count,retired_missing_count",
                          "((retired_through >= 0) AND (retired_count >= 0) AND (retired_missing_count >= 0) AND ((retired_count + retired_missing_count) = retired_through) AND (retired_through <= observed_through) AND (retired_count <= captured_count) AND (retired_missing_count <= missing_count))");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_last_retention_check", "last_retention",
                          "((last_retention IS NULL) OR ((jsonb_typeof(last_retention) = 'object'::text) AND (octet_length((last_retention)::text) <= 65536)))");
                add_check(value, "retrieval_journal_producers", "retrieval_journal_producers_epoch_check", "producer_epoch", "(producer_epoch > 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_id_check", "id", "(id > 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_sequence_check", "sequence", "(sequence > 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_record_hash_check", "record_hash",
                          "(record_hash ~ '^[0-9a-f]{64}$'::text)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_operation_check", "operation",
                          "((operation = 'search'::text) OR (operation = 'brief'::text) OR (operation = 'pack'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_selector_kind_check", "selector_kind",
                          "((selector_kind = 'route'::text) OR (selector_kind = 'publication'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_selector_id_check", "selector_id",
                          "(selector_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_observed_at_check", "observed_at_unix_ms",
                          "(observed_at_unix_ms >= 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_status_check", "status",
                          "((status >= 100) AND (status <= 599))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_reason_check", "status,reason_code",
                          "(((status = 200) AND (reason_code IS NULL)) OR ((status <> 200) AND (reason_code IS NOT NULL) AND (reason_code ~ '^[a-z][a-z0-9-]{0,63}$'::text)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_last_phase_check", "last_phase",
                          "((last_phase = 'validation'::text) OR (last_phase = 'selection'::text) OR (last_phase = 'admission'::text) OR (last_phase = 'execution'::text) OR (last_phase = 'finalization'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_elapsed_check", "elapsed_ns", "(elapsed_ns >= 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_result_bytes_check", "result_bytes",
                          "((result_bytes IS NULL) OR (result_bytes >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_validation_ns_check", "validation_ns", "(validation_ns >= 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_selection_ns_check", "selection_ns",
                          "((selection_ns IS NULL) OR (selection_ns >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_admission_ns_check", "admission_ns",
                          "((admission_ns IS NULL) OR (admission_ns >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_execution_ns_check", "execution_ns",
                          "((execution_ns IS NULL) OR (execution_ns >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_finalization_ns_check", "finalization_ns",
                          "((finalization_ns IS NULL) OR (finalization_ns >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_phase_presence_check",
                          "last_phase,selection_ns,admission_ns,execution_ns,finalization_ns",
                          "(((last_phase = 'validation'::text) AND (selection_ns IS NULL) AND (admission_ns IS NULL) AND (execution_ns IS NULL) AND (finalization_ns IS NULL)) OR ((last_phase = 'selection'::text) AND (selection_ns IS NOT NULL) AND (admission_ns IS NULL) AND (execution_ns IS NULL) AND (finalization_ns IS NULL)) OR ((last_phase = 'admission'::text) AND (selection_ns IS NOT NULL) AND (admission_ns IS NOT NULL) AND (execution_ns IS NULL) AND (finalization_ns IS NULL)) OR ((last_phase = 'execution'::text) AND (selection_ns IS NOT NULL) AND (admission_ns IS NOT NULL) AND (execution_ns IS NOT NULL) AND (finalization_ns IS NULL)) OR ((last_phase = 'finalization'::text) AND (selection_ns IS NOT NULL) AND (admission_ns IS NOT NULL) AND (execution_ns IS NOT NULL) AND (finalization_ns IS NOT NULL)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_phase_sum_check",
                          "elapsed_ns,validation_ns,selection_ns,admission_ns,execution_ns,finalization_ns",
                          "(elapsed_ns = ((((validation_ns + COALESCE(selection_ns, (0)::bigint)) + COALESCE(admission_ns, (0)::bigint)) + COALESCE(execution_ns, (0)::bigint)) + COALESCE(finalization_ns, (0)::bigint)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_route_id_check", "route_id",
                          "((route_id IS NULL) OR (route_id ~ '^[a-z][a-z0-9-]{0,62}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_workspace_id_check", "workspace_id",
                          "((workspace_id IS NULL) OR (workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_source_host_id_check", "source_host_id",
                          "((source_host_id IS NULL) OR (source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_snapshot_id_check", "snapshot_id",
                          "((snapshot_id IS NULL) OR (snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_preparation_id_check", "preparation_id",
                          "((preparation_id IS NULL) OR (preparation_id >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_revision_check", "revision",
                          "((revision IS NULL) OR (revision >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_profile_id_check", "profile_id",
                          "((profile_id IS NULL) OR (profile_id ~ '^[a-z][a-z0-9-]{0,62}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_profile_fingerprint_check", "profile_fingerprint",
                          "((profile_fingerprint IS NULL) OR (profile_fingerprint ~ '^fnv1a64:[0-9a-f]{16}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_route_fingerprint_check", "route_fingerprint",
                          "((route_fingerprint IS NULL) OR (route_fingerprint ~ '^[0-9a-f]{64}$'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_retrieval_mode_check", "retrieval_mode",
                          "((retrieval_mode IS NULL) OR (retrieval_mode = 'strict'::text) OR (retrieval_mode = 'relaxed'::text) OR (retrieval_mode = 'owner-scoped'::text) OR (retrieval_mode = 'directory-scoped'::text) OR (retrieval_mode = 'planned'::text) OR (retrieval_mode = 'identifier-scoped'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_returned_records_check", "returned_records",
                          "((returned_records IS NULL) OR (returned_records >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_candidate_rows_check", "candidate_rows",
                          "((candidate_rows IS NULL) OR (candidate_rows >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_covered_groups_check", "covered_groups",
                          "((covered_groups IS NULL) OR (covered_groups >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_total_groups_check", "total_groups",
                          "((total_groups IS NULL) OR (total_groups >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_required_requirements_check", "required_requirements",
                          "((required_requirements IS NULL) OR (required_requirements >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_satisfied_check", "satisfied_required_requirements",
                          "((satisfied_required_requirements IS NULL) OR (satisfied_required_requirements >= 0))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_query_capture_check", "query_capture",
                          "((query_capture = 'disabled'::text) OR (query_capture = 'invalid-request'::text) OR (query_capture = 'captured'::text) OR (query_capture = 'omitted-budget'::text) OR (query_capture = 'capture-failed'::text))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_uncaptured_request_check",
                          "query_capture,request_fingerprint,request",
                          "((query_capture = 'captured'::text) OR ((request IS NULL) AND (request_fingerprint IS NULL)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_captured_request_check",
                          "query_capture,request_fingerprint,request",
                          "((query_capture <> 'captured'::text) OR ((request IS NOT NULL) AND (jsonb_typeof(request) = 'object'::text) AND (octet_length((request)::text) <= 16384) AND (request_fingerprint IS NOT NULL) AND (request_fingerprint ~ '^[0-9a-f]{64}$'::text)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_epoch_check", "producer_epoch", "(producer_epoch > 0)");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_scope_check", "scope_principal_id,scope_tenant_id,scope_workspace_id",
                    "(((scope_principal_id IS NULL) AND (scope_tenant_id IS NULL) AND (scope_workspace_id IS NULL)) OR "
                    "((scope_principal_id IS NOT NULL) AND (scope_tenant_id IS NOT NULL) AND (scope_workspace_id IS NOT NULL)))");
                add_check(value, "retrieval_journal_attempts", "retrieval_journal_attempts_selection_scope_check", "workspace_id,scope_workspace_id",
                    "((scope_workspace_id IS NULL) OR (workspace_id IS NULL) OR (workspace_id = scope_workspace_id))");
                add_check(value, "retrieval_journal_lifecycle", "retrieval_journal_lifecycle_singleton_check", "singleton", "singleton");
                add_check(value, "retrieval_journal_lifecycle", "retrieval_journal_lifecycle_progress_check",
                          "allocated_through,reclaimed_through,reclaimed_producer_count,reclaimed_captured_count,reclaimed_missing_count",
                          "((allocated_through >= 0) AND (reclaimed_through >= 0) AND (reclaimed_through <= allocated_through) AND (reclaimed_producer_count >= 0) AND (reclaimed_producer_count <= reclaimed_through) AND (reclaimed_captured_count >= 0) AND (reclaimed_missing_count >= 0))");
                add_check(value, "retrieval_journal_lifecycle", "retrieval_journal_lifecycle_receipt_check", "last_reclamation",
                          "((last_reclamation IS NULL) OR ((jsonb_typeof(last_reclamation) = 'object'::text) AND (octet_length((last_reclamation)::text) <= 65536)))");
                return value;
            }();
            std::set<Constraint> actual;
            for (const auto& row : rows) {
                if (!row[7].as<bool>() || !row[8].as<bool>() || !row[9].as<bool>()) unsupported("constraint properties");
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<std::string>(),
                               row[4].as<std::string>(), row[5].as<std::string>(), row[6].as<std::string>());
            }
            if (actual != expected) unsupported("constraints");
        });
    checks.add(
        "SELECT t.relname,i.relname, "
        "array_to_string(ARRAY(SELECT a.attname FROM unnest(x.indkey) WITH ORDINALITY k(num,pos) "
        "JOIN pg_attribute a ON a.attrelid=x.indrelid AND a.attnum=k.num ORDER BY k.pos),','), "
        "x.indisunique,x.indisprimary,COALESCE(pg_get_expr(x.indpred,x.indrelid),''), "
        "x.indisvalid AND x.indisready AND x.indislive AND x.indimmediate AND x.indexprs IS NULL "
        "AND x.indnkeyatts=x.indnatts AND am.amname='btree' "
        "FROM pg_index x JOIN pg_class i ON i.oid=x.indexrelid JOIN pg_class t ON t.oid=x.indrelid "
        "JOIN pg_namespace n ON n.oid=t.relnamespace JOIN pg_am am ON am.oid=i.relam "
        "WHERE n.nspname='shin_server' AND t.relname IN ('retrieval_journal_producers','retrieval_journal_attempts','retrieval_journal_lifecycle')",
        [](const pqxx::result& rows) {
            static const std::set<Index> expected{
                {"retrieval_journal_producers","retrieval_journal_producers_pkey","instance_id",true,true,""},
                {"retrieval_journal_producers","retrieval_journal_producers_epoch_unique","producer_epoch",true,false,""},
                {"retrieval_journal_producers","retrieval_journal_producers_instance_epoch_unique","instance_id,producer_epoch",true,false,""},
                {"retrieval_journal_attempts","retrieval_journal_attempts_pkey","id",true,true,""},
                {"retrieval_journal_attempts","retrieval_journal_attempts_instance_sequence_unique","instance_id,sequence",true,false,""},
                {"retrieval_journal_attempts","retrieval_journal_attempts_project_id_id_idx","project_id,id",false,false,""},
                {"retrieval_journal_attempts","retrieval_journal_attempts_scope_cursor_idx","scope_principal_id,project_id,scope_tenant_id,scope_workspace_id,id",false,false,"(scope_principal_id IS NOT NULL)"},
                {"retrieval_journal_lifecycle","retrieval_journal_lifecycle_pkey","singleton",true,true,""},
            };
            std::set<Index> actual;
            for (const auto& row : rows) {
                if (!row[6].as<bool>()) unsupported("index properties");
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                               row[3].as<bool>(), row[4].as<bool>(), row[5].as<std::string>());
            }
            if (actual != expected) unsupported("indexes");
        });
    checks.add(
        "SELECT count(*)=1 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_sequence s ON s.seqrelid=c.oid JOIN pg_depend d ON d.objid=c.oid AND d.classid='pg_class'::regclass "
        "JOIN pg_attribute a ON a.attrelid=d.refobjid AND a.attnum=d.refobjsubid "
        "WHERE n.nspname='shin_server' AND c.relkind='S' "
        "AND s.seqtypid='bigint'::regtype AND s.seqstart=1 AND s.seqincrement=1 AND s.seqmin=1 "
        "AND s.seqmax=9223372036854775807 AND NOT s.seqcycle AND d.deptype='i' "
        "AND d.refclassid='pg_class'::regclass AND c.relname='retrieval_journal_attempts_id_seq' "
        "AND d.refobjid='shin_server.retrieval_journal_attempts'::regclass "
        "AND a.attname='id' AND a.attidentity='a'",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported("identity sequences");
        });
}
} // namespace shin::server
