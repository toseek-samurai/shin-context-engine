#include "shin/server/publication_schema.h"
#include "shin/server/model.h"

#include <pqxx/pqxx>
#include <set>
#include <string>
#include <tuple>

namespace shin::server {
namespace {
[[noreturn]] void unsupported() {
    throw ServiceError(503, "schema-unsupported", "Publication schema is invalid.");
}
} // namespace

void append_publication_schema_checks(SchemaChecks& checks) {
    checks.add(
        "SELECT c.relname,a.attnum,a.attname,format_type(a.atttypid,a.atttypmod),a.attnotnull,a.attidentity, "
        "COALESCE(pg_get_expr(d.adbin,d.adrelid),'') "
        "FROM pg_attribute a JOIN pg_class c ON c.oid=a.attrelid JOIN pg_namespace n ON n.oid=c.relnamespace "
        "LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
        "WHERE n.nspname='shin_server' AND c.relkind='r' "
        "AND c.relname IN ('publication_proposals','publication_resolutions','publication_heads','publication_withdrawals','publication_history','workspace_publication_history') AND a.attnum>0 AND NOT a.attisdropped",
        [](const pqxx::result& rows) {
            using Column = std::tuple<std::string, int, std::string, std::string, bool, std::string, std::string>;
            static const std::set<Column> expected{
                {"publication_proposals",1,"id","bigint",true,"a",""},
                {"publication_proposals",2,"project_id","text",true,"",""},
                {"publication_proposals",3,"publication_id","text",true,"",""},
                {"publication_proposals",4,"preparation_id","bigint",true,"",""},
                {"publication_proposals",5,"base_revision","bigint",true,"",""},
                {"publication_proposals",6,"base_preparation_id","bigint",false,"",""},
                {"publication_proposals",7,"submitted_by","text",true,"",""},
                {"publication_proposals",8,"idempotency_key","text",true,"",""},
                {"publication_proposals",9,"request_hash","text",true,"",""},
                {"publication_proposals",10,"created_at","timestamp with time zone",true,"","clock_timestamp()"},
                {"publication_proposals",11,"source_route_id","text",true,"",""},
                {"publication_proposals",12,"workspace_id","text",true,"",""},
                {"publication_proposals",13,"source_host_id","text",true,"",""},
                {"publication_proposals",14,"profile_id","text",true,"",""},
                {"publication_proposals",15,"route_fingerprint","text",true,"",""},
                {"publication_proposals",16,"base_withdrawal_id","bigint",false,"",""},
                {"publication_proposals",17,"history_epoch","bigint",true,"","0"},
                {"publication_proposals",18,"base_source_route_id","text",false,"",""},
                {"publication_proposals",19,"base_workspace_id","text",false,"",""},
                {"publication_proposals",20,"base_source_host_id","text",false,"",""},
                {"publication_proposals",21,"base_profile_id","text",false,"",""},
                {"publication_proposals",22,"base_route_fingerprint","text",false,"",""},
                {"publication_proposals",23,"history_workspace_id","text",false,"",""},
                {"workspace_publication_history",1,"project_id","text",true,"",""},
                {"workspace_publication_history",2,"workspace_id","text",true,"",""},
                {"workspace_publication_history",3,"history_epoch","bigint",true,"","0"},
                {"workspace_publication_history",4,"retired_proposals","bigint",true,"","0"},
                {"workspace_publication_history",5,"last_retention","jsonb",false,"",""},
                {"publication_history",1,"singleton","boolean",true,"","true"},
                {"publication_history",2,"history_epoch","bigint",true,"","0"},
                {"publication_history",3,"retired_proposals","bigint",true,"","0"},
                {"publication_history",4,"last_retention","jsonb",false,"",""},
                {"publication_withdrawals",1,"id","bigint",true,"a",""},
                {"publication_withdrawals",2,"project_id","text",true,"",""},
                {"publication_withdrawals",3,"publication_id","text",true,"",""},
                {"publication_withdrawals",4,"expected_revision","bigint",true,"",""},
                {"publication_withdrawals",5,"revision","bigint",true,"",""},
                {"publication_withdrawals",6,"previous_proposal_id","bigint",true,"",""},
                {"publication_withdrawals",7,"previous_preparation_id","bigint",true,"",""},
                {"publication_withdrawals",8,"previous_decision","text",true,"","'accept'::text"},
                {"publication_withdrawals",9,"withdrawn_by","text",true,"",""},
                {"publication_withdrawals",10,"created_at","timestamp with time zone",true,"","clock_timestamp()"},
                {"publication_resolutions",1,"proposal_id","bigint",true,"",""},
                {"publication_resolutions",2,"project_id","text",true,"",""},
                {"publication_resolutions",3,"publication_id","text",true,"",""},
                {"publication_resolutions",4,"decision","text",true,"",""},
                {"publication_resolutions",5,"expected_revision","bigint",true,"",""},
                {"publication_resolutions",6,"revision","bigint",true,"",""},
                {"publication_resolutions",7,"preparation_id","bigint",true,"",""},
                {"publication_resolutions",8,"created_at","timestamp with time zone",true,"","clock_timestamp()"},
                {"publication_resolutions",9,"resolved_by","text",true,"",""},
                {"publication_heads",1,"project_id","text",true,"",""},
                {"publication_heads",2,"publication_id","text",true,"",""},
                {"publication_heads",3,"revision","bigint",true,"",""},
                {"publication_heads",4,"preparation_id","bigint",true,"",""},
                {"publication_heads",5,"snapshot_id","text",true,"",""},
                {"publication_heads",6,"route_fingerprint","text",true,"",""},
                {"publication_heads",7,"proposal_id","bigint",true,"",""},
                {"publication_heads",8,"decision","text",true,"","'accept'::text"}};
            std::set<Column> actual;
            for (const auto& row : rows)
                actual.emplace(row[0].as<std::string>(), row[1].as<int>(), row[2].as<std::string>(), row[3].as<std::string>(),
                               row[4].as<bool>(), row[5].as<std::string>(), row[6].as<std::string>());
            if (actual != expected) unsupported();
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
        "WHERE n.nspname='shin_server' AND c.contype IN ('p','u','f','c') AND "
        "(t.relname IN ('publication_proposals','publication_resolutions','publication_heads','publication_withdrawals','publication_history','workspace_publication_history') "
        "OR (t.relname='preparations' AND c.conname='preparations_publication_identity_unique') "
        "OR (t.relname='audit_events' AND c.conname='audit_publication_proposal_fk'))",
        [](const pqxx::result& rows) {
            using Constraint = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, std::string>;
            static const std::set<Constraint> expected{
                {"preparations","preparations_publication_identity_unique","u","project_id,id","","",""},
                {"publication_proposals","publication_proposals_pkey","p","id","","",""},
                {"publication_proposals","publication_proposals_project_identity_unique","u","project_id,id","","",""},
                {"publication_proposals","publication_proposals_identity_unique","u","project_id,publication_id,id,preparation_id","","",""},
                {"publication_proposals","publication_proposals_project_fk","f","project_id","shin_server.projects","id",""},
                {"publication_proposals","publication_proposals_preparation_fk","f","project_id,preparation_id","shin_server.preparations","project_id,id",""},
                {"publication_proposals","publication_proposals_base_fk","f","project_id,base_preparation_id","shin_server.preparations","project_id,id",""},
                {"publication_proposals","publication_proposals_source_fk","f","project_id,source_route_id,preparation_id","shin_server.preparations","project_id,route_id,id",""},
                {"publication_proposals","publication_proposals_workspace_fk","f","project_id,workspace_id","shin_server.workspaces","project_id,id",""},
                {"publication_history","publication_history_pkey","p","singleton","","",""},
                {"workspace_publication_history","workspace_publication_history_pkey","p","project_id,workspace_id","","",""},
                {"workspace_publication_history","workspace_publication_history_project_id_workspace_id_fkey","f","project_id,workspace_id",
                 "shin_server.identity_workspace_owners","project_id,workspace_id",""},
                {"workspace_publication_history","workspace_publication_history_history_epoch_check","c","history_epoch","","","(history_epoch >= 0)"},
                {"workspace_publication_history","workspace_publication_history_retired_proposals_check","c","retired_proposals","","","(retired_proposals >= 0)"},
                {"workspace_publication_history","workspace_publication_history_last_retention_check","c","last_retention","","",
                 "((last_retention IS NULL) OR ((jsonb_typeof(last_retention) = 'object'::text) AND (octet_length((last_retention)::text) <= 65536)))"},
                {"publication_proposals","publication_history_workspace_check","c","workspace_id,history_workspace_id","","",
                 "((history_workspace_id IS NULL) OR (history_workspace_id = workspace_id))"},
                {"publication_proposals","publication_history_workspace_fk","f","project_id,history_workspace_id",
                 "shin_server.workspace_publication_history","project_id,workspace_id",""},
                {"publication_history","publication_history_singleton_check","c","singleton","","","singleton"},
                {"publication_history","publication_history_epoch_check","c","history_epoch","","","(history_epoch >= 0)"},
                {"publication_history","publication_history_count_check","c","retired_proposals","","","(retired_proposals >= 0)"},
                {"publication_history","publication_history_receipt_check","c","last_retention","","",
                 "((last_retention IS NULL) OR ((jsonb_typeof(last_retention) = 'object'::text) AND (octet_length((last_retention)::text) <= 65536)))"},
                {"publication_proposals","publication_proposals_epoch_check","c","history_epoch","","","(history_epoch >= 0)"},
                {"publication_proposals","publication_proposals_base_origin_check","c",
                 "base_preparation_id,base_source_route_id,base_workspace_id,base_source_host_id,base_profile_id,base_route_fingerprint","","",
                 "(((base_preparation_id IS NULL) = (base_source_route_id IS NULL)) AND ((base_preparation_id IS NULL) = (base_workspace_id IS NULL)) "
                 "AND ((base_preparation_id IS NULL) = (base_source_host_id IS NULL)) AND ((base_preparation_id IS NULL) = (base_profile_id IS NULL)) "
                 "AND ((base_preparation_id IS NULL) = (base_route_fingerprint IS NULL)))"},
                {"publication_proposals","publication_proposals_base_route_check","c","base_source_route_id","","",
                 "(base_source_route_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_base_workspace_check","c","base_workspace_id","","",
                 "(base_workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_base_host_check","c","base_source_host_id","","",
                 "(base_source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_base_profile_check","c","base_profile_id","","",
                 "(base_profile_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_base_fingerprint_check","c","base_route_fingerprint","","",
                 "(base_route_fingerprint ~ '^[0-9a-f]{64}$'::text)"},
                {"publication_proposals","publication_proposals_id_check","c","id","","","(id > 0)"},
                {"publication_proposals","publication_proposals_publication_check","c","publication_id","","","(publication_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_preparation_check","c","preparation_id","","","(preparation_id > 0)"},
                {"publication_proposals","publication_proposals_base_check","c","base_revision,base_preparation_id,base_withdrawal_id","","",
                 "(((base_revision = 0) AND (base_preparation_id IS NULL) AND (base_withdrawal_id IS NULL)) OR "
                 "((base_revision > 0) AND (base_preparation_id IS NOT NULL) AND (base_preparation_id > 0) AND (base_withdrawal_id IS NULL)) OR "
                 "((base_revision > 0) AND (base_preparation_id IS NULL) AND (base_withdrawal_id IS NOT NULL) AND (base_withdrawal_id > 0)))"},
                {"publication_proposals","publication_proposals_author_check","c","submitted_by","","","(submitted_by ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_key_check","c","idempotency_key","","","(idempotency_key ~ '^[A-Za-z0-9._:-]{1,128}$'::text)"},
                {"publication_proposals","publication_proposals_hash_check","c","request_hash","","","(request_hash ~ '^[0-9a-f]{64}$'::text)"},
                {"publication_proposals","publication_proposals_source_route_check","c","source_route_id","","","(source_route_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_workspace_check","c","workspace_id","","","(workspace_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_source_host_check","c","source_host_id","","","(source_host_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_profile_check","c","profile_id","","","(profile_id ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_proposals","publication_proposals_fingerprint_check","c","route_fingerprint","","","(route_fingerprint ~ '^[0-9a-f]{64}$'::text)"},
                {"publication_resolutions","publication_resolutions_pkey","p","proposal_id","","",""},
                {"publication_resolutions","publication_resolutions_head_unique","u","proposal_id,project_id,publication_id,decision,revision,preparation_id","","",""},
                {"publication_resolutions","publication_resolutions_proposal_fk","f","project_id,publication_id,proposal_id,preparation_id",
                 "shin_server.publication_proposals","project_id,publication_id,id,preparation_id",""},
                {"publication_resolutions","publication_resolutions_decision_check","c","decision","","",
                 "((decision = 'accept'::text) OR (decision = 'reject'::text) OR (decision = 'request-reprepare'::text))"},
                {"publication_resolutions","publication_resolutions_expected_check","c","expected_revision","","",
                 "((expected_revision >= 0) AND (expected_revision < '9223372036854775807'::bigint))"},
                {"publication_resolutions","publication_resolutions_revision_check","c","decision,expected_revision,revision","","",
                 "(((decision = 'accept'::text) AND (revision > 0) AND ((revision - 1) = expected_revision)) OR ((decision <> 'accept'::text) AND (revision = expected_revision)))"},
                {"publication_resolutions","publication_resolutions_author_check","c","resolved_by","","","(resolved_by ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_withdrawals","publication_withdrawals_pkey","p","id","","",""},
                {"publication_withdrawals","publication_withdrawals_previous_unique","u","previous_proposal_id","","",""},
                {"publication_withdrawals","publication_withdrawals_request_unique","u","project_id,publication_id,expected_revision","","",""},
                {"publication_withdrawals","publication_withdrawals_revision_unique","u","project_id,publication_id,revision","","",""},
                {"publication_withdrawals","publication_withdrawals_base_unique","u","project_id,publication_id,revision,id","","",""},
                {"publication_withdrawals","publication_withdrawals_resolution_fk","f",
                 "previous_proposal_id,project_id,publication_id,previous_decision,expected_revision,previous_preparation_id",
                 "shin_server.publication_resolutions","proposal_id,project_id,publication_id,decision,revision,preparation_id",""},
                {"publication_withdrawals","publication_withdrawals_id_check","c","id","","","(id > 0)"},
                {"publication_withdrawals","publication_withdrawals_expected_check","c","expected_revision","","",
                 "((expected_revision > 0) AND (expected_revision < '9223372036854775807'::bigint))"},
                {"publication_withdrawals","publication_withdrawals_revision_check","c","expected_revision,revision","","",
                 "((revision > 0) AND ((revision - 1) = expected_revision))"},
                {"publication_withdrawals","publication_withdrawals_decision_check","c","previous_decision","","","(previous_decision = 'accept'::text)"},
                {"publication_withdrawals","publication_withdrawals_author_check","c","withdrawn_by","","","(withdrawn_by ~ '^[a-z][a-z0-9-]{0,62}$'::text)"},
                {"publication_heads","publication_heads_pkey","p","project_id,publication_id","","",""},
                {"publication_heads","publication_heads_preparation_fk","f","project_id,preparation_id","shin_server.preparations","project_id,id",""},
                {"publication_heads","publication_heads_resolution_fk","f","proposal_id,project_id,publication_id,decision,revision,preparation_id",
                 "shin_server.publication_resolutions","proposal_id,project_id,publication_id,decision,revision,preparation_id",""},
                {"publication_heads","publication_heads_revision_check","c","revision","","","(revision > 0)"},
                {"publication_heads","publication_heads_snapshot_check","c","snapshot_id","","","(snapshot_id ~ '^fnv1a64:[0-9a-f]{16}$'::text)"},
                {"publication_heads","publication_heads_fingerprint_check","c","route_fingerprint","","","(route_fingerprint ~ '^[0-9a-f]{64}$'::text)"},
                {"publication_heads","publication_heads_decision_check","c","decision","","","(decision = 'accept'::text)"},
                {"audit_events","audit_publication_proposal_fk","f","project_id,publication_proposal_id","shin_server.publication_proposals","project_id,id",""}};
            std::set<Constraint> actual;
            for (const auto& row : rows) {
                if (!row[7].as<bool>() || !row[8].as<bool>() || !row[9].as<bool>()) unsupported();
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<std::string>(),
                               row[4].as<std::string>(), row[5].as<std::string>(), row[6].as<std::string>());
            }
            if (actual != expected) unsupported();
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
        "WHERE n.nspname='shin_server' AND (t.relname IN ('publication_proposals','publication_resolutions','publication_heads','publication_withdrawals','publication_history','workspace_publication_history') "
        "OR (t.relname='preparations' AND i.relname='preparations_publication_identity_unique'))",
        [](const pqxx::result& rows) {
            using Index = std::tuple<std::string, std::string, std::string, bool, bool, std::string>;
            static const std::set<Index> expected{
                {"preparations","preparations_publication_identity_unique","project_id,id",true,false,""},
                {"publication_proposals","publication_proposals_pkey","id",true,true,""},
                {"publication_history","publication_history_pkey","singleton",true,true,""},
                {"workspace_publication_history","workspace_publication_history_pkey","project_id,workspace_id",true,true,""},
                {"publication_proposals","publication_proposals_project_identity_unique","project_id,id",true,false,""},
                {"publication_proposals","publication_proposals_identity_unique","project_id,publication_id,id,preparation_id",true,false,""},
                {"publication_proposals","publication_proposals_legacy_request_unique","submitted_by,idempotency_key",true,false,"(history_workspace_id IS NULL)"},
                {"publication_proposals","publication_proposals_workspace_request_unique","submitted_by,project_id,history_workspace_id,idempotency_key",true,false,"(history_workspace_id IS NOT NULL)"},
                {"publication_proposals","publication_proposals_page_idx","project_id,publication_id,id",false,false,""},
                {"publication_proposals","publication_proposals_preparation_idx","preparation_id",false,false,""},
                {"publication_proposals","publication_proposals_base_idx","base_preparation_id",false,false,""},
                {"publication_proposals","publication_proposals_withdrawal_idx","base_withdrawal_id",false,false,""},
                {"publication_resolutions","publication_resolutions_pkey","proposal_id",true,true,""},
                {"publication_resolutions","publication_resolutions_head_unique","proposal_id,project_id,publication_id,decision,revision,preparation_id",true,false,""},
                {"publication_resolutions","publication_resolutions_accepted_idx","project_id,publication_id,revision",true,false,"(decision = 'accept'::text)"},
                {"publication_withdrawals","publication_withdrawals_pkey","id",true,true,""},
                {"publication_withdrawals","publication_withdrawals_previous_unique","previous_proposal_id",true,false,""},
                {"publication_withdrawals","publication_withdrawals_request_unique","project_id,publication_id,expected_revision",true,false,""},
                {"publication_withdrawals","publication_withdrawals_revision_unique","project_id,publication_id,revision",true,false,""},
                {"publication_withdrawals","publication_withdrawals_base_unique","project_id,publication_id,revision,id",true,false,""},
                {"publication_heads","publication_heads_pkey","project_id,publication_id",true,true,""},
                {"publication_heads","publication_heads_preparation_idx","preparation_id",false,false,""}};
            std::set<Index> actual;
            for (const auto& row : rows) {
                if (!row[6].as<bool>()) unsupported();
                actual.emplace(row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                               row[3].as<bool>(), row[4].as<bool>(), row[5].as<std::string>());
            }
            if (actual != expected) unsupported();
        });
    checks.add("SELECT count(*)=1 FROM shin_server.publication_history WHERE singleton",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported();
        });
    checks.add(
        "SELECT count(*)=2 FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "JOIN pg_sequence s ON s.seqrelid=c.oid JOIN pg_depend d ON d.objid=c.oid AND d.classid='pg_class'::regclass "
        "JOIN pg_attribute a ON a.attrelid=d.refobjid AND a.attnum=d.refobjsubid "
        "WHERE n.nspname='shin_server' AND c.relkind='S' "
        "AND s.seqtypid='bigint'::regtype AND s.seqstart=1 AND s.seqincrement=1 AND s.seqmin=1 "
        "AND s.seqmax=9223372036854775807 AND NOT s.seqcycle AND d.deptype='i' "
        "AND d.refclassid='pg_class'::regclass AND "
        "((c.relname='publication_proposals_id_seq' AND d.refobjid='shin_server.publication_proposals'::regclass) OR "
        "(c.relname='publication_withdrawals_id_seq' AND d.refobjid='shin_server.publication_withdrawals'::regclass)) "
        "AND a.attname='id' AND a.attidentity='a'",
        [](const pqxx::result& rows) {
            if (rows.size() != 1 || !rows[0][0].as<bool>()) unsupported();
        });
}
} // namespace shin::server
