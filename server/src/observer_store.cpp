#include "shin/server/store.h"
#include "legacy_project_access.h"
#include "shin/server/identity.h"
#include "shin/server/identity_directory.h"
#include "shin/server/config.h"
#include "shin/server/observer_ingestion.h"
#include "shin/observer_protocol.h"
#include "observer_store_internal.h"
#include <pqxx/pqxx>
#include <algorithm>
#include <optional>

namespace shin::server {
namespace observer_store_detail {
bool hex(const Json& value, std::size_t size) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void authorize(const Principal& principal, bool admin) {
    legacy_access::validate_principal(principal);
    if (admin) legacy_access::require_admin(principal);
}

void admit(pqxx::transaction_base& tx, const Principal& principal, const std::optional<ObserverScope>& scope) {
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
    if (scope) {
        if (principal.admin) throw ServiceError(403, "forbidden", "Personal Observer operations require an owner identity.");
        (void)authorize_identity_workspace(tx, principal, scope->workspace.tenant_id, scope->project_id, scope->workspace.workspace_id);
        tx.exec("SELECT set_config('shin.observer_ops','0',true),set_config('shin.observer_legacy','0',true),"
                "set_config('shin.observer_quota','1',true)");
    } else {
        std::string projects;
        for (const auto& project : principal.project_ids) { if (!projects.empty()) projects += ','; projects += project; }
        tx.exec("SELECT set_config('shin.identity_principal',$1,true),set_config('shin.identity_legacy_projects',$2,true),"
                "set_config('shin.identity_ops',$3,true),set_config('shin.observer_ops',$3,true),"
                "set_config('shin.observer_legacy','1',true),set_config('shin.observer_quota','1',true)",
                pqxx::params{tx, principal.id, projects, principal.admin ? "1" : "0"});
        if (!principal.admin && (!tx.exec("SELECT 1 FROM shin_server.identity_accounts WHERE principal_id=$1", pqxx::params{tx, principal.id}).empty() ||
            !tx.exec("SELECT 1 FROM shin_server.identity_project_owners WHERE project_id=ANY($1::text[]) LIMIT 1", pqxx::params{tx, principal.project_ids}).empty()))
            throw ServiceError(403, "identity-data-plane-not-enabled", "Use the personal workspace Observer route.");
    }
}

pqxx::result find_stream_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream, Lock lock) {
    if (!hex(stream, 32)) throw ServiceError(400, "invalid-request", "Invalid stream identity.");
    auto rows = tx.exec("SELECT * FROM shin_server.observer_streams WHERE stream_id=$1 "
                        "AND ($2 OR (principal_id=$3 AND (scope_tenant_id IS NOT NULL OR project_id=ANY($4::text[]))))" +
                        std::string(lock == Lock::exclusive ? " FOR UPDATE" : lock == Lock::shared ? " FOR SHARE" : ""),
                        pqxx::params{tx, stream, principal.admin, principal.id, principal.project_ids});
    return rows;
}

pqxx::result tombstone_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream) {
    return tx.exec("SELECT * FROM shin_server.observer_tombstones WHERE stream_id=$1 "
        "AND ($2 OR (principal_id=$3 AND (scope_tenant_id IS NOT NULL OR project_id=ANY($4::text[]))))",
        pqxx::params{tx, stream, principal.admin, principal.id, principal.project_ids});
}

pqxx::result stream_rows(pqxx::transaction_base& tx, const Principal& principal, const std::string& stream, Lock lock) {
    auto rows = find_stream_rows(tx, principal, stream, lock);
    if (rows.empty()) {
        if (!tombstone_rows(tx, principal, stream).empty())
            throw ServiceError(410, "observer-stream-retired", "Stream identity is permanently retired; explicit new enrollment is required.");
        throw ServiceError(404, "observer-not-found", "Observer stream was not found.");
    }
    return rows;
}

Json stream_json(const pqxx::row_ref& row) {
    return {{"stream_id", row["stream_id"].template as<std::string>()},
        {"project_id", row["project_id"].template as<std::string>()},
        {"workspace_id", row["workspace_id"].template as<std::string>()},
        {"principal_id", row["principal_id"].template as<std::string>()},
        {"observer_id", row["observer_id"].template as<std::string>()},
        {"source_host_id", row["source_host_id"].template as<std::string>()},
        {"profile", row["profile"].template as<std::string>()},
        {"workspace_binding_digest", row["binding_digest"].template as<std::string>()},
        {"revoked", row["revoked"].template as<bool>()},
        {"accepted_sequence", row["accepted_sequence"].template as<std::int64_t>()},
        {"confirmed_sequence", row["confirmed_sequence"].as<std::int64_t>()},
        {"retired_sequence", row["retired_sequence"].as<std::int64_t>()},
        {"retained_submissions", row["accepted_sequence"].as<std::int64_t>() - row["retired_sequence"].as<std::int64_t>()},
        {"observed_sequence", row["observed_sequence"].template as<std::int64_t>()},
        {"retained_bytes", row["retained_bytes"].template as<std::int64_t>()},
        {"verification_level", "producer-observed"}, {"server_source_verified", false},
        {"shared_publication_mutated", false}};
}

void audit(pqxx::transaction_base& tx, const Principal& principal, const pqxx::row_ref& stream,
           const std::string& action, std::optional<std::int64_t> sequence) {
    tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.audit-writers.v1')),set_config('shin.audit_append','1',true)");
    tx.exec("INSERT INTO shin_server.audit_events(principal_id,action,project_id,workspace_id,observer_stream_id,observer_sequence) "
            "VALUES ($1,$2,$3,$4,$5,$6)", pqxx::params{tx, principal.id, action,
            stream["project_id"].template as<std::string>(), stream["workspace_id"].template as<std::string>(),
            stream["stream_id"].template as<std::string>(), sequence});
}
} // namespace observer_store_detail
using namespace observer_store_detail;

void validate_enrollment(const Json& value) {
    require_fields(value, {"stream_id", "project_id", "workspace_id", "principal_id", "observer_id", "source_host_id", "profile", "workspace_binding_digest"});
    for (const auto* key : {"project_id", "workspace_id", "principal_id", "observer_id", "source_host_id", "profile"})
        if (!valid_identifier(require_text(value, key, 63)))
            throw ServiceError(400, "invalid-request", "Invalid Observer binding.");
    if (!hex(value.at("stream_id"), 32) || !hex(value.at("workspace_binding_digest"), 64))
        throw ServiceError(400, "invalid-request", "Invalid Observer binding digest.");
}

Json Store::enroll_observer(const Principal& principal, const Json& input, const std::optional<ObserverScope>& scope) {
    authorize(principal, !scope);
    validate_enrollment(input);
    if (scope && (input.at("principal_id") != principal.id || input.at("project_id") != scope->project_id ||
                  input.at("workspace_id") != scope->workspace.workspace_id))
        throw ServiceError(403, "observer-binding-mismatch", "Enrollment must match the authenticated workspace owner.");
    const auto stream = input.at("stream_id").get<std::string>();
    const auto project = input.at("project_id").get<std::string>();
    const auto workspace = input.at("workspace_id").get<std::string>();
    const auto hash = digest_hex(sha256(scope ? Json::array({scope->workspace.tenant_id, input}).dump() : input.dump()));
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto binding = tx.exec("SELECT source_host_id FROM shin_server.workspaces WHERE project_id=$1 AND id=$2",
                                    pqxx::params{tx, project, workspace});
        if (binding.empty()) throw ServiceError(404, "workspace-not-found", "Workspace was not found.");
        if (binding[0]["source_host_id"].as<std::string>() != input.at("source_host_id").get<std::string>())
            throw ServiceError(409, "workspace-host-mismatch", "Observer source host does not match the workspace.");
        // One quota row fences bounded registry admission; replay precedes capacity rejection.
        const auto usage = tx.exec("SELECT streams FROM shin_server.observer_usage WHERE singleton FOR UPDATE");
        if (usage.size() != 1) throw ServiceError(503, "observer-store-unavailable", "Observer storage is unavailable.");
        if (!tombstone_rows(tx, principal, stream).empty())
            throw ServiceError(410, "observer-stream-retired", "A retired stream identity cannot be enrolled again.");
        auto previous = tx.exec("SELECT * FROM shin_server.observer_streams WHERE stream_id=$1", pqxx::params{tx, stream});
        if (!previous.empty()) {
            if (previous[0]["enrollment_hash"].as<std::string>() != hash)
                throw ServiceError(409, "observer-enrollment-conflict", "Stream identity is already bound differently.");
            return Json{{"stream", stream_json(previous[0])}, {"replayed", true}};
        }
        // Enrollment owns quota before inserting. Reject a live identity without
        // waiting on its unique-index conflict while a stream writer needs quota.
        const auto active = tx.exec("SELECT 1 FROM shin_server.observer_streams WHERE project_id=$1 AND workspace_id=$2 AND observer_id=$3 AND NOT revoked",
            pqxx::params{tx, project, workspace, input.at("observer_id").get<std::string>()});
        if (!active.empty()) throw ServiceError(409, "observer-enrollment-conflict", "Observer identity already has an active stream.");
        // Fixed registry checks count hidden legacy entries without returning their payloads.
        const auto prior = tx.exec("SELECT current_setting('shin.observer_ops',true)")[0][0].as<std::string>("");
        tx.exec("SELECT set_config('shin.observer_ops','1',true)");
        const auto count = tx.exec("SELECT count(*) FROM shin_server.observer_streams WHERE project_id=$1", pqxx::params{tx, project});
        const auto retired = tx.exec("SELECT 1 FROM shin_server.observer_tombstones WHERE stream_id=$1", pqxx::params{tx, stream});
        tx.exec("SELECT set_config('shin.observer_ops',$1,true)", pqxx::params{tx, prior});
        if (!retired.empty()) throw ServiceError(409, "observer-enrollment-conflict", "Stream identity is unavailable.");
        if (usage[0]["streams"].as<std::int64_t>() >= 1024 || count[0][0].as<std::int64_t>() >= 128)
            throw ServiceError(409, "observer-capacity", "Observer registry capacity was reached.");
        const auto rows = tx.exec("INSERT INTO shin_server.observer_streams "
            "(stream_id,project_id,workspace_id,principal_id,observer_id,source_host_id,profile,binding_digest,enrollment_hash,scope_tenant_id) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10) ON CONFLICT DO NOTHING RETURNING *",
            pqxx::params{tx, stream, project, workspace, input.at("principal_id").get<std::string>(),
            input.at("observer_id").get<std::string>(), input.at("source_host_id").get<std::string>(),
            input.at("profile").get<std::string>(), input.at("workspace_binding_digest").get<std::string>(), hash,
            scope ? std::optional<std::string>(scope->workspace.tenant_id) : std::nullopt});
        if (rows.empty()) throw ServiceError(409, "observer-enrollment-conflict", "Observer identity already has an active stream.");
        tx.exec("UPDATE shin_server.observer_usage SET streams=streams+1 WHERE singleton");
        audit(tx, principal, rows[0], "observer.enrolled");
        return Json{{"stream", stream_json(rows[0])}, {"replayed", false}};
    });
}

Json Store::get_observer(const Principal& principal, const std::string& stream, const std::optional<ObserverScope>& scope) {
    authorize(principal);
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        return stream_json(stream_rows(tx, principal, stream, Lock::none)[0]);
    });
}

Json Store::observer_identity(const Principal& principal) {
    authorize(principal);
    return run_transaction([&](pqxx::transaction_base& tx) {
        const auto identity = read_server_identity(tx);
        return Json{{"schema_version", observer_protocol::kIdentitySchema},
            {"request_identity_header", observer_protocol::kRequestIdentityHeader},
                    {"server_id", identity},
                    {"principal_id", principal.id},
                    {"submission_schema", "shin.observer-submission.v1"},
                    {"acknowledgement_schema", "shin.observer-ack.v1"},
                    {"confirmation_schema", "shin.observer-confirmation.v1"},
                    {"reconciliation_schema", "shin.observer-reconcile-result.v1"},
                    {"verification_level", "service-observed"},
                    {"source_verified", false},
                    {"shared_publication_mutated", false}};
    });
}

Json Store::revoke_observer(const Principal& principal, const std::string& stream, const std::optional<ObserverScope>& scope) {
    authorize(principal, !scope);
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::exclusive);
        if (rows[0]["revoked"].as<bool>()) return Json{{"stream", stream_json(rows[0])}, {"replayed", true}};
        const auto updated = tx.exec("UPDATE shin_server.observer_streams SET revoked=true WHERE stream_id=$1 RETURNING *",
                                     pqxx::params{tx, stream});
        audit(tx, principal, updated[0], "observer.revoked");
        return Json{{"stream", stream_json(updated[0])}, {"replayed", false}};
    });
}

Json Store::ingest_observation(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope) {
    authorize(principal);
    try { shin::observer_protocol::verify_submission(input); }
    catch (const shin::observer_protocol::ValidationError&) {
        throw ServiceError(400, "invalid-submission", "Observer submission is invalid.");
    }
    if (input.at("stream_id") != stream) throw ServiceError(403, "observer-binding-mismatch", "Submission does not match its stream.");
    const auto sequence = input.at("sequence").get<std::int64_t>();
    const auto hash = digest_hex(sha256(input.dump()));
    const auto& payload = input.at("payload");
    return run_transaction([&](pqxx::transaction_base& tx) {
        admit(tx, principal, scope);
        const auto rows = stream_rows(tx, principal, stream, Lock::exclusive);
        const auto& row = rows[0];
        if (row["principal_id"].as<std::string>() != principal.id)
            throw ServiceError(403, "observer-binding-mismatch", "Only the enrolled producer may submit observations.");
        if (row["revoked"].as<bool>()) throw ServiceError(403, "observer-revoked", "Observer stream is revoked.");
        for (const auto* key : {"project_id", "workspace_id", "observer_id", "source_host_id", "profile"})
            if (payload.at(key) != row[key].as<std::string>())
                throw ServiceError(403, "observer-binding-mismatch", "Submission binding was not enrolled.");
        if (payload.at("workspace_binding_digest") != row["binding_digest"].as<std::string>())
            throw ServiceError(403, "observer-binding-mismatch", "Submission binding was not enrolled.");
        if (sequence <= row["retired_sequence"].as<std::int64_t>())
            throw ServiceError(410, "observer-history-retired", "Submission identity belongs to retired history; explicit reconciliation is required.");
        const auto previous = tx.exec("SELECT submission_hash,receipt::text FROM shin_server.observer_submissions WHERE stream_id=$1 AND sequence=$2",
                                      pqxx::params{tx, stream, sequence});
        if (!previous.empty()) {
            if (previous[0]["submission_hash"].as<std::string>() != hash)
                throw ServiceError(409, "submission-conflict", "Submission identity already has different content.");
            return Json{{"receipt", Json::parse(previous[0]["receipt"].as<std::string>())}, {"replayed", true}};
        }
        const auto accepted = row["accepted_sequence"].as<std::int64_t>();
        const auto observed = payload.at("observer_sequence").get<std::int64_t>();
        if (sequence != accepted + 1 || observed <= row["observed_sequence"].as<std::int64_t>())
            throw ServiceError(409, "observer-sequence-gap", "Submission ordering requires reconciliation.");
        if (accepted - row["retired_sequence"].as<std::int64_t>() >= 1024)
            throw ServiceError(409, "observer-capacity", "Retained stream history capacity was reached.");
        const auto identity = read_server_identity(tx);
        const auto received = tx.exec("SELECT to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')")[0][0].as<std::string>();
        const Json receipt{{"schema_version", "shin.observer-ack.v1"}, {"server_id", identity},
            {"stream_id", stream}, {"sequence", sequence}, {"submission_id", input.at("submission_id")},
            {"payload_sha256", input.at("payload_sha256")}, {"project_id", row["project_id"].as<std::string>()},
            {"workspace_id", row["workspace_id"].as<std::string>()}, {"received_at", received},
            {"verification_level", "producer-observed"}, {"server_source_verified", false}, {"shared_publication_mutated", false}};
        const auto bytes = static_cast<std::int64_t>(input.dump().size() + receipt.dump().size());
        if (bytes > 12288 || bytes > 4194304 - row["retained_bytes"].as<std::int64_t>())
            throw ServiceError(409, "observer-capacity", "Stream history byte capacity was reached.");
        const auto reserved = tx.exec("UPDATE shin_server.observer_usage SET submissions=submissions+1,retained_bytes=retained_bytes+$1 "
            "WHERE singleton AND submissions<65536 AND retained_bytes<=134217728-$1 RETURNING singleton", pqxx::params{tx, bytes});
        if (reserved.empty()) throw ServiceError(409, "observer-capacity", "Observer history capacity was reached.");
        tx.exec("INSERT INTO shin_server.observer_submissions(stream_id,sequence,submission_id,submission_hash,payload_hash,submission,receipt,retained_bytes,received_at) "
            "VALUES ($1,$2,$3,$4,$5,$6::jsonb,$7::jsonb,$8,$9::timestamptz)", pqxx::params{tx, stream, sequence,
            input.at("submission_id").get<std::string>(), hash, input.at("payload_sha256").get<std::string>(), input.dump(), receipt.dump(), bytes, received});
        tx.exec("UPDATE shin_server.observer_streams SET accepted_sequence=$2,observed_sequence=$3,retained_bytes=retained_bytes+$4 "
            "WHERE stream_id=$1", pqxx::params{tx, stream, sequence, observed, bytes});
        audit(tx, principal, row, "observer.submission.accepted", sequence);
        return Json{{"receipt", receipt}, {"replayed", false}};
    });
}
} // namespace shin::server
