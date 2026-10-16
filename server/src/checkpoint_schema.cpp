#include "shin/server/checkpoint_schema.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>

#include <initializer_list>
#include <algorithm>
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

[[noreturn]] void unsupported(const char* detail) {
    throw ServiceError(503, "schema-unsupported", std::string("Private checkpoint schema ") + detail + " is not supported.");
}

} // namespace

void append_checkpoint_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT p.polname,p.polcmd,pg_get_expr(p.polqual,p.polrelid),COALESCE(pg_get_expr(p.polwithcheck,p.polrelid),''), "
        "p.polpermissive AND p.polroles=ARRAY[0]::oid[] AND c.relrowsecurity AND c.relforcerowsecurity "
        "AND c.relowner=n.nspowner FROM pg_policy p JOIN pg_class c ON c.oid=p.polrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='shin_server' AND c.relname='checkpoint_slots'",
        [](const pqxx::result& rows) {
            const auto compact = [](std::string value) {
                std::erase_if(value, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
                return value;
            };
            using Policy = std::tuple<std::string, std::string, std::string, std::string>;
            const std::string ops = "(current_setting('shin.identity_ops'::text, true) = '1'::text)";
            const std::string scope =
                "((owner_id = current_setting('shin.identity_principal'::text, true)) AND "
                "(project_id = current_setting('shin.checkpoint_project'::text, true)) AND "
                "(((scope_workspace_id IS NULL) AND (current_setting('shin.checkpoint_legacy'::text, true) = '1'::text)) OR "
                "((scope_workspace_id = current_setting('shin.checkpoint_workspace'::text, true)) AND "
                "(EXISTS ( SELECT 1 FROM (shin_server.identity_workspace_owners w "
                "JOIN shin_server.identity_accounts a ON ((a.id = w.owner_id))) "
                "WHERE ((w.project_id = checkpoint_slots.project_id) AND (w.workspace_id = checkpoint_slots.scope_workspace_id) "
                "AND (w.tenant_id = current_setting('shin.identity_tenant'::text, true)) "
                "AND (a.principal_id = checkpoint_slots.owner_id) AND a.enabled))))))";
            const std::set<Policy> expected{
                {"checkpoint_slots_ops", "*", compact(ops), compact(ops)},
                {"checkpoint_slots_scope", "*", compact(scope), ""}};
            std::set<Policy> actual;
            for (const auto& row : rows) {
                if (!row[4].as<bool>()) unsupported("row security properties");
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(),
                               compact(row[2].as<std::string>()), compact(row[3].as<std::string>()));
            }
            if (actual != expected) unsupported("row security policies");
        });
    checks.add(
        "SELECT c.relname,a.attnum,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,a.attidentity, "
        "COALESCE(pg_get_expr(d.adbin,d.adrelid),'') FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid "
        "JOIN pg_namespace n ON n.oid=c.relnamespace LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
        "WHERE n.nspname='shin_server' AND c.relkind='r' AND c.relname IN ('checkpoint_slots','checkpoint_lifecycle') "
        "AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            static const std::set<Column> expected = [] {
                std::set<Column> value;
                add_columns(value, "checkpoint_slots", {
                    {"project_id", "text", true}, {"owner_id", "text", true}, {"slot_id", "text", true},
                    {"state", "text", true}, {"revision", "bigint", true}, {"task_id", "text", true},
                    {"thread_id", "text", true}, {"provenance", "jsonb", true}, {"checkpoint_text", "text", false},
                    {"checkpoint_sha256", "text", false}, {"checkpoint_id", "text", false}, {"captured_at", "bigint", false},
                    {"ttl_seconds", "bigint", false}, {"snapshot_id", "text", false}, {"last_operation", "text", true},
                    {"last_request_id", "text", true}, {"last_request_hash", "text", true},
                    {"last_expected_revision", "bigint", true}, {"last_request_deadline", "bigint", true},
                    {"retry_until", "bigint", true}, {"updated_at", "bigint", true},
                    {"receipt_origins", "jsonb", true, "", "'[]'::jsonb"},
                    {"scope_workspace_id", "text", false},
                });
                add_columns(value, "checkpoint_lifecycle", {
                    {"singleton", "boolean", true, "", "true"}, {"reject_deadlines_through", "bigint", true, "", "0"},
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
        "AND t.relname IN ('checkpoint_slots','checkpoint_lifecycle')",
        [](const pqxx::result& rows) {
            static const std::set<Constraint> expected = [] {
                std::set<Constraint> value{
                    {"checkpoint_slots", "checkpoint_slots_revision_unique", "u", "revision", "", "", ""},
                    {"checkpoint_slots", "checkpoint_slots_project_fk", "f", "project_id", "shin_server.projects", "id", ""},
                    {"checkpoint_slots", "checkpoint_slots_workspace_fk", "f", "project_id,scope_workspace_id", "shin_server.workspaces", "project_id,id", ""},
                    {"checkpoint_lifecycle", "checkpoint_lifecycle_pkey", "p", "singleton", "", "", ""},
                };
                add_check(value, "checkpoint_slots", "checkpoint_slots_scope_check", "project_id,provenance,scope_workspace_id",
                          "((scope_workspace_id IS NULL) OR ((((provenance ->> 'project_id'::text) = project_id) AND "
                          "((provenance ->> 'workspace_id'::text) = scope_workspace_id)) IS TRUE))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_owner_id_check", "owner_id",
                          "(owner_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)");
                add_check(value, "checkpoint_slots", "checkpoint_slots_slot_id_check", "slot_id",
                          "(slot_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)");
                add_check(value, "checkpoint_slots", "checkpoint_slots_state_check", "state",
                          "((state = 'active'::text) OR (state = 'cleared'::text))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_revision_check", "revision", "(revision > 0)");
                add_check(value, "checkpoint_slots", "checkpoint_slots_binding_check", "task_id,thread_id",
                          "((task_id ~ '^[!-~]{1,128}$'::text) AND (thread_id ~ '^[!-~]{1,128}$'::text))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_provenance_check", "provenance",
                          "((jsonb_typeof(provenance) = 'object'::text) AND (octet_length((provenance)::text) <= 4096))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_payload_check",
                          "state,checkpoint_text,checkpoint_sha256,checkpoint_id,captured_at,ttl_seconds,snapshot_id",
                          "(((state = 'active'::text) AND (checkpoint_text IS NOT NULL) AND "
                          "((octet_length(checkpoint_text) >= 1) AND (octet_length(checkpoint_text) <= 262144)) AND "
                          "(checkpoint_sha256 IS NOT NULL) AND (checkpoint_id IS NOT NULL) AND (captured_at IS NOT NULL) AND "
                          "(ttl_seconds IS NOT NULL) AND (snapshot_id IS NOT NULL)) OR ((state = 'cleared'::text) AND "
                          "(checkpoint_text IS NULL) AND (checkpoint_sha256 IS NULL) AND (checkpoint_id IS NULL) AND "
                          "(captured_at IS NULL) AND (ttl_seconds IS NULL) AND (snapshot_id IS NULL)))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_checkpoint_fields_check",
                          "checkpoint_sha256,checkpoint_id,captured_at,ttl_seconds,snapshot_id",
                          "(((checkpoint_sha256 IS NULL) OR (checkpoint_sha256 ~ '^[0-9a-f]{64}$'::text)) AND "
                          "((checkpoint_id IS NULL) OR (checkpoint_id ~ '^fnv1a64:[0-9a-f]{16}$'::text)) AND "
                          "((captured_at IS NULL) OR ((captured_at >= 0) AND (captured_at <= '253402300799'::bigint))) AND "
                          "((ttl_seconds IS NULL) OR ((ttl_seconds >= 1) AND (ttl_seconds <= 604800))) AND "
                          "((snapshot_id IS NULL) OR (snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'::text)))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_receipt_check",
                          "last_operation,last_request_id,last_request_hash,last_expected_revision,last_request_deadline,retry_until",
                          "(((last_operation = 'save'::text) OR (last_operation = 'clear'::text) OR (last_operation = 'receipt-import'::text)) AND "
                          "(last_request_id ~ '^[0-9a-f]{32}$'::text) AND (last_request_hash ~ '^[0-9a-f]{64}$'::text) AND "
                          "(last_expected_revision >= 0) AND ((last_request_deadline >= 0) AND "
                          "(last_request_deadline <= '253402300799'::bigint)) AND ((retry_until >= last_request_deadline) AND "
                          "(retry_until <= '253402300799'::bigint)))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_updated_at_check", "updated_at",
                          "((updated_at >= 0) AND (updated_at <= '253402300799'::bigint))");
                add_check(value, "checkpoint_slots", "checkpoint_slots_origins_check", "state,receipt_origins",
                          "((jsonb_typeof(receipt_origins) = 'array'::text) AND (jsonb_array_length(receipt_origins) <= 32) AND "
                          "(octet_length((receipt_origins)::text) <= 2304) AND ((state = 'active'::text) OR (receipt_origins = '[]'::jsonb)))");
                add_check(value, "checkpoint_lifecycle", "checkpoint_lifecycle_singleton_check", "singleton", "singleton");
                add_check(value, "checkpoint_lifecycle", "checkpoint_lifecycle_deadline_floor_check", "reject_deadlines_through",
                          "((reject_deadlines_through >= 0) AND (reject_deadlines_through <= '253402300799'::bigint))");
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
        "WHERE n.nspname='shin_server' AND t.relname IN ('checkpoint_slots','checkpoint_lifecycle')",
        [](const pqxx::result& rows) {
            static const std::set<Index> expected{
                {"checkpoint_slots", "checkpoint_slots_legacy_unique", "project_id,owner_id,slot_id", true, false, "(scope_workspace_id IS NULL)"},
                {"checkpoint_slots", "checkpoint_slots_workspace_unique", "project_id,owner_id,scope_workspace_id,slot_id", true, false, "(scope_workspace_id IS NOT NULL)"},
                {"checkpoint_slots", "checkpoint_slots_revision_unique", "revision", true, false, ""},
                {"checkpoint_slots", "checkpoint_slots_prune_idx", "project_id,owner_id,scope_workspace_id,state,retry_until", false, false, ""},
                {"checkpoint_lifecycle", "checkpoint_lifecycle_pkey", "singleton", true, true, ""},
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
        "SELECT c.relowner=n.nspowner AS schema_owned,s.seqtypid='bigint'::regtype AS bigint_sequence, "
        "s.seqstart=1 AND s.seqincrement=1 AND s.seqmin=1 AND s.seqmax=9223372036854775807 AND NOT s.seqcycle AS sequence_bounds, "
        "NOT EXISTS(SELECT 1 FROM pg_depend d WHERE d.classid='pg_class'::regclass AND d.objid=c.oid "
        "AND d.refclassid='pg_class'::regclass AND d.refobjsubid<>0 AND d.deptype IN ('a','i')) AS standalone "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace JOIN pg_sequence s ON s.seqrelid=c.oid "
        "WHERE n.nspname='shin_server' AND c.relkind='S' AND c.relname='checkpoint_revisions'",
        [](const pqxx::result& rows) {
            if (rows.size() != 1) unsupported("revision sequence");
            for (const auto& value : rows[0]) if (!value.as<bool>()) unsupported("revision sequence");
        });
    checks.add(
        "SELECT count(*)=1 AND min(reject_deadlines_through)>=0 AND max(reject_deadlines_through)<=253402300799 "
        "FROM shin_server.checkpoint_lifecycle WHERE singleton",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported("lifecycle row");
        });
}

void verify_checkpoint_runtime_grants(pqxx::transaction_base& transaction, std::string_view runtime_role) {
    const auto rows = transaction.exec(
        "SELECT "
        "has_schema_privilege($1,'shin_server','USAGE') AS schema_usage, "
        "has_table_privilege($1,'shin_server.checkpoint_slots','SELECT') AS slots_select, "
        "has_table_privilege($1,'shin_server.checkpoint_slots','INSERT') AS slots_insert, "
        "has_table_privilege($1,'shin_server.checkpoint_slots','UPDATE') AS slots_update, "
        "has_table_privilege($1,'shin_server.checkpoint_slots','DELETE') AS slots_delete, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_slots','TRUNCATE') AS slots_no_truncate, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_slots','REFERENCES') AS slots_no_references, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_slots','TRIGGER') AS slots_no_trigger, "
        "has_table_privilege($1,'shin_server.checkpoint_lifecycle','SELECT') AS lifecycle_select, "
        "has_table_privilege($1,'shin_server.checkpoint_lifecycle','UPDATE') AS lifecycle_update, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_lifecycle','INSERT') AS lifecycle_no_insert, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_lifecycle','DELETE') AS lifecycle_no_delete, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_lifecycle','TRUNCATE') AS lifecycle_no_truncate, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_lifecycle','REFERENCES') AS lifecycle_no_references, "
        "NOT has_table_privilege($1,'shin_server.checkpoint_lifecycle','TRIGGER') AS lifecycle_no_trigger, "
        "has_sequence_privilege($1,'shin_server.checkpoint_revisions','USAGE') AS revisions_usage, "
        "NOT has_sequence_privilege($1,'shin_server.checkpoint_revisions','SELECT') AS revisions_no_select, "
        "NOT has_sequence_privilege($1,'shin_server.checkpoint_revisions','UPDATE') AS revisions_no_update",
        pqxx::params{transaction, std::string(runtime_role)});
    if (rows.size() != 1) unsupported("runtime grants");
    for (const auto& value : rows[0]) if (!value.as<bool>()) unsupported("runtime grants");
}

} // namespace shin::server
