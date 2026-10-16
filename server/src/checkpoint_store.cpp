#include "shin/server/store.h"
#include "route_access.h"
#include "shin/server/checkpoint_schema.h"
#include "shin/server/config.h"
#include "shin/server/identity_directory.h"

#include <pqxx/pqxx>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

namespace shin::server {
namespace {

constexpr std::int64_t kMaximumTimestamp = 253402300799;
constexpr std::int64_t kMaximumTtlSeconds = 604800;
constexpr std::size_t kMaximumListLimit = 32;
constexpr std::string_view kSlotColumns =
    "project_id,owner_id,slot_id,state,revision,task_id,thread_id,provenance::text AS provenance,"
    "checkpoint_text,checkpoint_sha256,checkpoint_id,captured_at,ttl_seconds,snapshot_id,"
    "last_operation,last_request_id,last_request_hash,last_expected_revision,last_request_deadline,retry_until,updated_at,"
    "receipt_origins::text AS receipt_origins";

struct CheckpointPayload {
    std::string checkpoint_id;
    std::string profile_id;
    std::string snapshot_id;
    std::int64_t captured_at = 0;
    std::int64_t ttl_seconds = 0;
};

[[noreturn]] void invalid_request() {
    throw ServiceError(400, "invalid-request", "Checkpoint request is invalid.");
}

[[noreturn]] void invalid_cursor() {
    throw ServiceError(400, "invalid-cursor", "The checkpoint cursor is invalid.");
}

[[noreturn]] void invalid_limit() {
    throw ServiceError(400, "invalid-limit", "The requested page size is invalid.");
}

[[noreturn]] void corrupt() {
    throw ServiceError(500, "database-corrupt", "Stored checkpoint data is invalid.");
}

[[noreturn]] void request_conflict() {
    throw ServiceError(409, "checkpoint-request-conflict", "Checkpoint request identity was used for a different mutation.");
}

[[noreturn]] void revision_conflict() {
    throw ServiceError(409, "checkpoint-revision-conflict", "Checkpoint revision changed.");
}

[[noreturn]] void source_scope_changed() {
    throw ServiceError(409, "checkpoint-source-scope-changed", "Checkpoint belongs to a different source scope.");
}

[[noreturn]] void checkpoint_not_found() {
    throw ServiceError(404, "checkpoint-not-found", "Private checkpoint was not found.");
}

[[noreturn]] void checkpoint_cleared() {
    throw ServiceError(409, "checkpoint-cleared", "Private checkpoint was explicitly cleared.");
}

[[noreturn]] void checkpoint_capacity() {
    throw ServiceError(507, "checkpoint-capacity", "Private checkpoint capacity was reached.");
}

[[noreturn]] void request_expired() {
    throw ServiceError(409, "checkpoint-request-expired", "Checkpoint request deadline expired.");
}

bool lowercase_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool fnv_id(std::string_view value) {
    return value.size() == 24 && value.starts_with("fnv1a64:") && lowercase_hex(value.substr(8), 16);
}

bool valid_slot(std::string_view value) {
    return valid_identifier(value) && value != "prune";
}

bool no_nul(const Json& root) {
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto* value = pending.back();
        pending.pop_back();
        if (value->is_string()) {
            if (value->get_ref<const std::string&>().find('\0') != std::string::npos) return false;
        } else if (value->is_array()) {
            for (const auto& item : *value) pending.push_back(&item);
        } else if (value->is_object()) {
            for (const auto& [key, item] : value->items()) {
                if (key.find('\0') != std::string::npos) return false;
                pending.push_back(&item);
            }
        }
    }
    return true;
}

bool bounded_integer(const Json& value, std::int64_t minimum, std::int64_t maximum, std::int64_t* output = nullptr) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum) || number < static_cast<std::uint64_t>(minimum)) return false;
        if (output != nullptr) *output = static_cast<std::int64_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < minimum || number > maximum) return false;
        if (output != nullptr) *output = number;
        return true;
    }
    return false;
}

bool text_field(const Json& object, std::string_view field, std::string* output) {
    const auto item = object.find(field);
    if (item == object.end() || !item->is_string()) return false;
    *output = item->get<std::string>();
    return true;
}

bool same_active_source(const Json& stored, const Json& current) {
    for (const auto* field : {"project_id", "workspace_id", "source_host_id", "route_id", "profile_id", "selector"}) {
        if (stored.at(field) != current.at(field)) return false;
    }
    return true;
}

bool parse_checkpoint_payload(const std::string& checkpoint, const CheckpointMutation& mutation,
                              const Json& provenance, CheckpointPayload* payload) {
    if (checkpoint.empty() || checkpoint.size() > kCheckpointMaximumBytes || checkpoint.find('\0') != std::string::npos) return false;
    try {
        const auto document = parse_bounded_json(checkpoint, kCheckpointMaximumBytes);
        if (!document.is_object() || !no_nul(document) || document.value("schema_version", "") != "shin.session-checkpoint.v1") {
            return false;
        }
        std::string checkpoint_id;
        std::string task_id;
        std::string thread_id;
        std::string profile_id;
        std::string snapshot_id;
        if (!text_field(document, "checkpoint_id", &checkpoint_id) || !fnv_id(checkpoint_id) ||
            !document.contains("binding") || !document.at("binding").is_object() ||
            !text_field(document.at("binding"), "task_id", &task_id) ||
            !text_field(document.at("binding"), "thread_id", &thread_id) ||
            !text_field(document.at("binding"), "profile", &profile_id) || !valid_identifier(profile_id) ||
            !document.contains("baseline") || !document.at("baseline").is_object() ||
            !text_field(document.at("baseline"), "snapshot_id", &snapshot_id) || !fnv_id(snapshot_id) ||
            task_id != mutation.task_id || thread_id != mutation.thread_id ||
            profile_id != provenance.at("profile_id").get<std::string>() || snapshot_id != provenance.at("snapshot_id").get<std::string>()) {
            return false;
        }
        std::int64_t captured_at = 0;
        std::int64_t ttl_seconds = 0;
        if (!document.contains("captured_at") || !bounded_integer(document.at("captured_at"), 0, kMaximumTimestamp, &captured_at) ||
            !document.contains("ttl_seconds") || !bounded_integer(document.at("ttl_seconds"), 1, kMaximumTtlSeconds, &ttl_seconds)) {
            return false;
        }
        *payload = {std::move(checkpoint_id), std::move(profile_id), std::move(snapshot_id), captured_at, ttl_seconds};
        return true;
    } catch (const ServiceError&) {
        return false;
    } catch (const Json::exception&) {
        return false;
    }
}

std::int64_t database_now(pqxx::transaction_base& tx) {
    const auto rows = tx.exec("SELECT floor(extract(epoch FROM clock_timestamp()))::bigint AS now");
    if (rows.size() != 1) corrupt();
    const auto now = rows[0]["now"].as<std::int64_t>();
    if (now < 0 || now > kMaximumTimestamp) corrupt();
    return now;
}

std::int64_t deadline_floor(pqxx::transaction_base& tx, bool lock) {
    const auto rows = tx.exec(std::string("SELECT reject_deadlines_through FROM shin_server.checkpoint_lifecycle WHERE singleton") +
        (lock ? " FOR UPDATE" : ""));
    if (rows.size() != 1) corrupt();
    const auto floor = rows[0]["reject_deadlines_through"].as<std::int64_t>();
    if (floor < 0 || floor > kMaximumTimestamp) corrupt();
    return floor;
}

void validate_deadline(const CheckpointMutation& mutation, std::int64_t now, std::int64_t floor) {
    if (mutation.request_deadline <= floor) request_expired();
    if (mutation.request_deadline <= now) request_expired();
    if (mutation.request_deadline > now + kCheckpointRetrySeconds) invalid_request();
}

template<class Row>
Json nullable_text(const Row& row, const char* field) {
    return row[field].is_null() ? Json(nullptr) : Json(row[field].template as<std::string>());
}

template<class Row>
Json nullable_integer(const Row& row, const char* field) {
    return row[field].is_null() ? Json(nullptr) : Json(row[field].template as<std::int64_t>());
}

void validate_receipt(const Json& record) {
    if ((record.at("last_operation") != "save" && record.at("last_operation") != "clear" &&
         record.at("last_operation") != "receipt-import") ||
        !record.at("last_request_id").is_string() || !lowercase_hex(record.at("last_request_id").get<std::string>(), 32) ||
        !record.at("last_request_hash").is_string() || !lowercase_hex(record.at("last_request_hash").get<std::string>(), 64) ||
        !bounded_integer(record.at("last_expected_revision"), 0, std::numeric_limits<std::int64_t>::max()) ||
        !bounded_integer(record.at("last_request_deadline"), 0, kMaximumTimestamp) ||
        !bounded_integer(record.at("retry_until"), 0, kMaximumTimestamp) ||
        record.at("retry_until").get<std::int64_t>() < record.at("last_request_deadline").get<std::int64_t>()) {
        corrupt();
    }
}

void validate_stored_record(const Json& record) {
    (void)checkpoint_public_record(record);
    if (!checkpoint_provenance_valid(record.at("provenance"), record.at("project_id").get<std::string>())) corrupt();
    validate_receipt(record);
    const bool active = record.at("state") == "active";
    if (!active) {
        if (!record.at("checkpoint_text").is_null() || !record.at("checkpoint_sha256").is_null() ||
            !record.at("checkpoint_id").is_null() || !record.at("captured_at").is_null() ||
            !record.at("ttl_seconds").is_null() || !record.at("snapshot_id").is_null() ||
            record.at("receipt_origins") != Json::array()) {
            corrupt();
        }
        return;
    }
    if (!record.at("checkpoint_text").is_string() || !record.at("checkpoint_sha256").is_string() ||
        !lowercase_hex(record.at("checkpoint_sha256").get<std::string>(), 64) || !record.at("snapshot_id").is_string()) {
        corrupt();
    }
    const auto& checkpoint = record.at("checkpoint_text").get_ref<const std::string&>();
    if (checkpoint.size() > kCheckpointMaximumBytes || checkpoint.find('\0') != std::string::npos ||
        digest_hex(sha256(checkpoint)) != record.at("checkpoint_sha256").get<std::string>()) {
        corrupt();
    }
    CheckpointMutation binding{record.at("task_id").get<std::string>(), record.at("thread_id").get<std::string>(),
                               record.at("last_request_id").get<std::string>(), record.at("last_request_hash").get<std::string>(),
                               record.at("last_expected_revision").get<std::uint64_t>(),
                               record.at("last_request_deadline").get<std::int64_t>()};
    CheckpointPayload payload;
    if (!parse_checkpoint_payload(checkpoint, binding, record.at("provenance"), &payload) ||
        payload.checkpoint_id != record.at("checkpoint_id").get<std::string>() ||
        payload.captured_at != record.at("captured_at").get<std::int64_t>() ||
        payload.ttl_seconds != record.at("ttl_seconds").get<std::int64_t>() ||
        payload.snapshot_id != record.at("snapshot_id").get<std::string>() ||
        !checkpoint_receipt_origins_valid(parse_bounded_json(checkpoint, kCheckpointMaximumBytes),
            record.at("owner_id").get<std::string>(), record.at("receipt_origins"))) {
        corrupt();
    }
}

template<class Row>
Json record_from_row(const Row& row) {
    Json provenance;
    Json origins;
    try {
        provenance = Json::parse(row["provenance"].template as<std::string>());
        origins = Json::parse(row["receipt_origins"].template as<std::string>());
    } catch (const Json::exception&) {
        corrupt();
    }
    Json record{{"schema_version", "shin.task-checkpoint.v1"},
                {"project_id", row["project_id"].template as<std::string>()},
                {"owner_id", row["owner_id"].template as<std::string>()},
                {"slot_id", row["slot_id"].template as<std::string>()},
                {"state", row["state"].template as<std::string>()},
                {"revision", row["revision"].template as<std::int64_t>()},
                {"task_id", row["task_id"].template as<std::string>()},
                {"thread_id", row["thread_id"].template as<std::string>()},
                {"provenance", std::move(provenance)},
                {"receipt_origins", std::move(origins)},
                {"checkpoint_text", nullable_text(row, "checkpoint_text")},
                {"checkpoint_sha256", nullable_text(row, "checkpoint_sha256")},
                {"checkpoint_id", nullable_text(row, "checkpoint_id")},
                {"captured_at", nullable_integer(row, "captured_at")},
                {"ttl_seconds", nullable_integer(row, "ttl_seconds")},
                {"snapshot_id", nullable_text(row, "snapshot_id")},
                {"last_operation", row["last_operation"].template as<std::string>()},
                {"last_request_id", row["last_request_id"].template as<std::string>()},
                {"last_request_hash", row["last_request_hash"].template as<std::string>()},
                {"last_expected_revision", row["last_expected_revision"].template as<std::int64_t>()},
                {"last_request_deadline", row["last_request_deadline"].template as<std::int64_t>()},
                {"retry_until", row["retry_until"].template as<std::int64_t>()},
                {"updated_at", row["updated_at"].template as<std::int64_t>()}};
    validate_stored_record(record);
    return record;
}

Json select_slot(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                 const std::string& slot, bool lock, const std::optional<WorkspaceReadRequest>& scope = std::nullopt) {
    const auto rows = tx.exec(std::string("SELECT ") + std::string(kSlotColumns) +
        " FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2 AND slot_id=$3 "
        "AND scope_workspace_id IS NOT DISTINCT FROM $4::text" +
        (lock ? " FOR UPDATE" : ""), pqxx::params{tx, project, principal.id, slot,
            scope ? std::optional(scope->workspace_id) : std::nullopt});
    if (rows.empty()) return Json(nullptr);
    if (rows.size() != 1) corrupt();
    return record_from_row(rows[0]);
}

bool receipt_matches(const Json& record, const CheckpointMutation& mutation, std::string_view operation) {
    if (record.at("last_request_id") != mutation.request_id) return false;
    if (record.at("last_request_hash") != mutation.request_hash || record.at("last_operation") != operation ||
        record.at("last_expected_revision") != mutation.expected_revision || record.at("task_id") != mutation.task_id ||
        record.at("thread_id") != mutation.thread_id) {
        request_conflict();
    }
    return true;
}

void require_task_thread(const Json& record, const CheckpointMutation& mutation) {
    if (record.at("task_id") != mutation.task_id || record.at("thread_id") != mutation.thread_id) checkpoint_not_found();
}

void require_expected_revision(const Json& record, const CheckpointMutation& mutation) {
    if (record.at("revision") != mutation.expected_revision) revision_conflict();
}

void validate_target(const std::string& project, const std::string& slot) {
    if (!valid_identifier(project) || !valid_slot(slot)) invalid_request();
}

void add_replay(Json& record) {
    record["replayed"] = true;
}

void add_committed(Json& record) {
    record["replayed"] = false;
}

Json updated_slot(pqxx::transaction_base& tx, const std::string& query, pqxx::params parameters) {
    const auto rows = tx.exec(query, parameters);
    if (rows.size() != 1) corrupt();
    return record_from_row(rows[0]);
}

void mutation_scope(pqxx::transaction_base& tx, const Principal& principal, const std::string& project,
                    const std::optional<WorkspaceReadRequest>& scope, const Json& expected = Json()) {
    tx.exec("SELECT set_config('shin.checkpoint_project',$1,true), set_config('shin.checkpoint_workspace',$2,true), "
            "set_config('shin.checkpoint_legacy',$3,true)",
            pqxx::params{tx, project, scope ? scope->workspace_id : "", scope ? "0" : "1"});
    if (!scope) {
        // Legacy callers have already passed get_project; never inherit pooled directory authority.
        tx.exec("SELECT set_config('shin.identity_principal',$1,true), set_config('shin.identity_ops','0',true), "
                "set_config('shin.identity_account','',true), set_config('shin.identity_tenant','',true), "
                "set_config('shin.identity_manage','',true), set_config('shin.identity_legacy_projects','',true)",
                pqxx::params{tx, principal.id});
        return;
    }
    tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.identity_directory.v1'))");
    const auto current = scoped_workspace_snapshot(tx, principal, project, *scope);
    if (!expected.is_null() && current != expected)
        throw ServiceError(409, "workspace-write-scope-changed", "Workspace authority changed; start a new checkpoint request.");
}

pqxx::result capacity_counts(pqxx::transaction_base& tx, const Principal& principal, const std::string& project) {
    // Trusted aggregate only, under the existing global mutation lock. Restore before any payload query.
    const auto previous = tx.exec("SELECT current_setting('shin.identity_ops',true)")[0][0].as<std::string>("");
    tx.exec("SELECT set_config('shin.identity_ops','1',true)");
    const auto counts = tx.exec(
        "SELECT (SELECT count(*) FROM shin_server.checkpoint_slots) AS total, "
        "(SELECT count(*) FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2) AS owner_total",
        pqxx::params{tx, project, principal.id});
    tx.exec("SELECT set_config('shin.identity_ops',$1,true)", pqxx::params{tx, previous});
    return counts;
}

void current_source(pqxx::transaction_base& tx, const std::string& project, const Json& provenance) {
    const auto& selector = provenance.at("selector");
    const auto id = selector.at("id").get<std::string>();
    const auto preparation = provenance.at("preparation_id").get<std::int64_t>();
    const auto revision = provenance.at("revision").get<std::int64_t>();
    const auto snapshot = provenance.at("snapshot_id").get<std::string>();
    bool current = false;
    if (selector.at("kind") == "publication") {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.publication-writers.v1'))");
        current = !tx.exec("SELECT 1 FROM shin_server.publication_heads h JOIN shin_server.publication_proposals p "
            "ON p.id=h.proposal_id AND p.project_id=h.project_id AND p.publication_id=h.publication_id "
            "AND p.preparation_id=h.preparation_id WHERE h.project_id=$1 AND h.publication_id=$2 "
            "AND h.preparation_id=$3 AND h.revision=$4 AND h.snapshot_id=$5 AND p.workspace_id=$6 "
            "AND p.source_route_id=$7 AND p.source_host_id=$8 AND p.profile_id=$9",
            pqxx::params{tx, project, id, preparation, revision, snapshot,
                provenance.at("workspace_id").get<std::string>(), provenance.at("route_id").get<std::string>(),
                provenance.at("source_host_id").get<std::string>(), provenance.at("profile_id").get<std::string>()}).empty();
    } else {
        tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.activation-writers.v1'))");
        select_content_route(tx, id);
        current = !tx.exec("SELECT 1 FROM shin_server.active_routes WHERE project_id=$1 AND route_id=$2 "
            "AND preparation_id=$3 AND revision=$4 AND snapshot_id=$5",
            pqxx::params{tx, project, id, preparation, revision, snapshot}).empty();
    }
    if (!current) throw ServiceError(409, "checkpoint-source-scope-changed", "Checkpoint source changed before commit; evaluate it again.");
}

} // namespace

Json Store::checkpoint_record(const Principal& principal, const std::string& project, const std::string& slot) {
    validate_target(project, slot);
    (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, std::nullopt);
        return select_slot(tx, principal, project, slot, false);
    });
}

Json Store::scoped_checkpoint_record(const Principal& principal, const std::string& project, const std::string& slot,
                                     const WorkspaceReadRequest& scope) {
    validate_target(project, slot);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        const auto rows = tx.exec(std::string("SELECT ") + std::string(kSlotColumns) +
            " FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2 AND slot_id=$3 "
            "AND scope_workspace_id=$4 AND provenance->>'project_id'=$1 AND provenance->>'workspace_id'=$4",
            pqxx::params{tx, project, principal.id, slot, scope.workspace_id});
        if (rows.empty()) return Json(nullptr);
        return record_from_row(rows[0]);
    });
}

Json Store::scoped_checkpoints(const Principal& principal, const std::string& project, const std::string& after,
                               std::size_t limit, const WorkspaceReadRequest& scope) {
    if (!valid_identifier(project)) invalid_request();
    if (!after.empty() && !valid_slot(after)) invalid_cursor();
    if (limit == 0 || limit > kMaximumListLimit) invalid_limit();
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        const auto observed_at = database_now(tx);
        const auto rows = tx.exec(std::string("SELECT ") + std::string(kSlotColumns) +
            " FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2 AND slot_id>$3 "
            "AND scope_workspace_id=$4 AND provenance->>'project_id'=$1 AND provenance->>'workspace_id'=$4 ORDER BY slot_id LIMIT $5",
            pqxx::params{tx, project, principal.id, after, scope.workspace_id, static_cast<std::int64_t>(limit + 1)});
        Json entries = Json::array();
        const auto page_limit = static_cast<pqxx::result::size_type>(limit);
        for (pqxx::result::size_type i = 0; i < std::min(page_limit, rows.size()); ++i) {
            auto entry = checkpoint_public_record(record_from_row(rows[i]));
            entry["observed_at"] = observed_at;
            entries.push_back(std::move(entry));
        }
        const Json next = rows.size() > page_limit ? entries.back().at("slot_id") : Json(nullptr);
        return Json{{"schema_version", "shin.task-checkpoint-inventory.v1"}, {"tenant_id", scope.tenant_id},
                    {"project_id", project}, {"workspace_id", scope.workspace_id}, {"entries", std::move(entries)}, {"next_cursor", next}};
    });
}

Json Store::checkpoint_replay(const Principal& principal, const std::string& project, const std::string& slot,
                              const CheckpointMutation& mutation, const std::string& operation,
                              const std::optional<WorkspaceReadRequest>& scope) {
    validate_target(project, slot);
    validate_checkpoint_mutation(mutation);
    if (operation != "save" && operation != "clear") invalid_request();
    if (!scope) (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        if (scope) tx.exec("SELECT pg_advisory_xact_lock_shared(hashtext('shin_server.checkpoint-mutations.v1'))");
        const auto now = database_now(tx);
        validate_deadline(mutation, now, deadline_floor(tx, false));
        auto record = select_slot(tx, principal, project, slot, false, scope);
        if (record.is_null()) {
            if (operation == "save" && mutation.expected_revision == 0) return Json(nullptr);
            if (operation == "clear") checkpoint_not_found();
            revision_conflict();
        }
        if (receipt_matches(record, mutation, operation)) {
            add_replay(record);
            return record;
        }
        if (record.at("state") == "active" || operation == "clear") require_task_thread(record, mutation);
        if (record.at("state") == "cleared" && operation == "clear") checkpoint_cleared();
        require_expected_revision(record, mutation);
        return Json(nullptr);
    });
}

Json Store::save_checkpoint(const Principal& principal, const std::string& project, const std::string& slot,
                            const CheckpointMutation& mutation, const Json& provenance, const std::string& checkpoint,
                            const std::optional<WorkspaceReadRequest>& scope, const Json& admission) {
    validate_target(project, slot);
    validate_checkpoint_mutation(mutation);
    if (!checkpoint_provenance_valid(provenance, project)) invalid_request();
    if (scope && (!admission.is_object() || provenance.at("workspace_id") != scope->workspace_id ||
        !bounded_integer(provenance.at("preparation_id"), 1, std::numeric_limits<std::int64_t>::max()) ||
        (provenance.at("selector").at("kind") == "route" && provenance.at("selector").at("id") != provenance.at("route_id"))))
        invalid_request();
    CheckpointPayload payload;
    if (!parse_checkpoint_payload(checkpoint, mutation, provenance, &payload)) invalid_request();
    const auto checkpoint_sha256 = digest_hex(sha256(checkpoint));
    if (!scope) (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope, admission);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.checkpoint-mutations.v1'))");
        const auto now = database_now(tx);
        validate_deadline(mutation, now, deadline_floor(tx, true));
        auto record = select_slot(tx, principal, project, slot, true, scope);
        // Exact replay acknowledges the prior commit, not current source quality.
        if (scope && (record.is_null() || !receipt_matches(record, mutation, "save"))) current_source(tx, project, provenance);
        if (!record.is_null()) {
            if (receipt_matches(record, mutation, "save")) {
                add_replay(record);
                return record;
            }
            if (record.at("state") == "active") {
                require_task_thread(record, mutation);
                require_expected_revision(record, mutation);
                if (!same_active_source(record.at("provenance"), provenance)) source_scope_changed();
            } else {
                require_expected_revision(record, mutation);
            }
            const auto updated = updated_slot(tx,
                std::string("UPDATE shin_server.checkpoint_slots SET state='active',revision=nextval('shin_server.checkpoint_revisions'),") +
                "task_id=$1,thread_id=$2,provenance=$3::jsonb,checkpoint_text=$4,checkpoint_sha256=$5,checkpoint_id=$6,receipt_origins='[]'::jsonb,"
                "captured_at=$7,ttl_seconds=$8,snapshot_id=$9,last_operation='save',last_request_id=$10,last_request_hash=$11,"
                "last_expected_revision=$12,last_request_deadline=$13,retry_until=GREATEST(retry_until,$14),updated_at=$15 "
                "WHERE project_id=$16 AND owner_id=$17 AND slot_id=$18 AND scope_workspace_id IS NOT DISTINCT FROM $19::text "
                "RETURNING " + std::string(kSlotColumns),
                pqxx::params{tx, mutation.task_id, mutation.thread_id, provenance.dump(), checkpoint, checkpoint_sha256,
                             payload.checkpoint_id, payload.captured_at, payload.ttl_seconds, payload.snapshot_id,
                             mutation.request_id, mutation.request_hash, static_cast<std::int64_t>(mutation.expected_revision),
                             mutation.request_deadline, mutation.request_deadline, now, project, principal.id, slot,
                             scope ? std::optional(scope->workspace_id) : std::nullopt});
            auto response = updated;
            add_committed(response);
            return response;
        }
        if (mutation.expected_revision != 0) revision_conflict();
        const auto counts = capacity_counts(tx, principal, project);
        if (counts.size() != 1) corrupt();
        if (counts[0]["total"].as<std::int64_t>() >= static_cast<std::int64_t>(kCheckpointMaximumSlots) ||
            counts[0]["owner_total"].as<std::int64_t>() >= static_cast<std::int64_t>(kCheckpointMaximumOwnerSlots)) {
            checkpoint_capacity();
        }
        const auto inserted = updated_slot(tx,
            std::string("INSERT INTO shin_server.checkpoint_slots(project_id,owner_id,slot_id,state,revision,task_id,thread_id,") +
            "provenance,checkpoint_text,checkpoint_sha256,checkpoint_id,captured_at,ttl_seconds,snapshot_id,last_operation,"
            "last_request_id,last_request_hash,last_expected_revision,last_request_deadline,retry_until,updated_at,scope_workspace_id) VALUES "
            "($1,$2,$3,'active',nextval('shin_server.checkpoint_revisions'),$4,$5,$6::jsonb,$7,$8,$9,$10,$11,$12,'save',$13,$14,$15,$16,$17,$18,$19) "
            "RETURNING " + std::string(kSlotColumns),
            pqxx::params{tx, project, principal.id, slot, mutation.task_id, mutation.thread_id, provenance.dump(), checkpoint,
                         checkpoint_sha256, payload.checkpoint_id, payload.captured_at, payload.ttl_seconds, payload.snapshot_id,
                         mutation.request_id, mutation.request_hash, static_cast<std::int64_t>(mutation.expected_revision),
                         mutation.request_deadline, mutation.request_deadline, now,
                         scope ? std::optional(scope->workspace_id) : std::nullopt});
        auto response = inserted;
        add_committed(response);
        return response;
    });
}

Json Store::clear_checkpoint(const Principal& principal, const std::string& project, const std::string& slot,
                             const CheckpointMutation& mutation, const std::optional<WorkspaceReadRequest>& scope) {
    validate_target(project, slot);
    validate_checkpoint_mutation(mutation);
    if (!scope) (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.checkpoint-mutations.v1'))");
        const auto now = database_now(tx);
        validate_deadline(mutation, now, deadline_floor(tx, true));
        auto record = select_slot(tx, principal, project, slot, true, scope);
        if (record.is_null()) checkpoint_not_found();
        if (receipt_matches(record, mutation, "clear")) {
            add_replay(record);
            return record;
        }
        require_task_thread(record, mutation);
        if (record.at("state") == "cleared") checkpoint_cleared();
        require_expected_revision(record, mutation);
        auto response = updated_slot(tx,
            std::string("UPDATE shin_server.checkpoint_slots SET state='cleared',revision=nextval('shin_server.checkpoint_revisions'),") +
            "checkpoint_text=NULL,checkpoint_sha256=NULL,checkpoint_id=NULL,captured_at=NULL,ttl_seconds=NULL,snapshot_id=NULL,receipt_origins='[]'::jsonb,"
            "last_operation='clear',last_request_id=$1,last_request_hash=$2,last_expected_revision=$3,last_request_deadline=$4,"
            "retry_until=GREATEST(retry_until,$4),updated_at=$5 WHERE project_id=$6 AND owner_id=$7 AND slot_id=$8 "
            "AND scope_workspace_id IS NOT DISTINCT FROM $9::text RETURNING " +
            std::string(kSlotColumns),
            pqxx::params{tx, mutation.request_id, mutation.request_hash, static_cast<std::int64_t>(mutation.expected_revision),
                         mutation.request_deadline, now, project, principal.id, slot,
                         scope ? std::optional(scope->workspace_id) : std::nullopt});
        add_committed(response);
        return response;
    });
}

Json Store::import_checkpoint_receipts(const Principal& principal, const std::string& project, const std::string& slot,
                                       const CheckpointMutation& mutation, const Json& receipts,
                                       const std::optional<WorkspaceReadRequest>& scope) {
    validate_target(project, slot);
    validate_checkpoint_mutation(mutation);
    if (!scope) (void)get_project(principal, project);
    if (!principal.receipt_producer)
        throw ServiceError(403, "receipt-import-forbidden", "Receipt producer permission is required.");
    if (!receipts.is_array() || receipts.empty() || receipts.size() > 32 || receipts.dump().size() > kCheckpointMaximumBytes)
        invalid_request();
    std::set<std::string> imported;
    for (const auto& receipt : receipts) {
        if (!receipt.is_object() || receipt.value("producer", Json()) != principal.id ||
            !imported.insert(digest_hex(sha256(receipt.dump()))).second) invalid_request();
    }
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.checkpoint-mutations.v1'))");
        const auto now = database_now(tx);
        validate_deadline(mutation, now, deadline_floor(tx, true));
        auto record = select_slot(tx, principal, project, slot, true, scope);
        if (record.is_null()) checkpoint_not_found();
        if (receipt_matches(record, mutation, "receipt-import")) {
            add_replay(record);
            return record;
        }
        require_task_thread(record, mutation);
        if (record.at("state") == "cleared") checkpoint_cleared();
        require_expected_revision(record, mutation);
        const auto checkpoint = parse_bounded_json(record.at("checkpoint_text").get<std::string>(), kCheckpointMaximumBytes);
        if (!checkpoint_receipt_origins_valid(checkpoint, principal.id, Json(imported)))
            throw ServiceError(409, "receipt-import-mismatch", "Receipts must exactly match saved records in this task scope.");
        auto merged = imported;
        for (const auto& origin : record.at("receipt_origins")) merged.insert(origin.get<std::string>());
        auto response = updated_slot(tx,
            std::string("UPDATE shin_server.checkpoint_slots SET receipt_origins=$1::jsonb,") +
            "revision=nextval('shin_server.checkpoint_revisions'),last_operation='receipt-import',last_request_id=$2,"
            "last_request_hash=$3,last_expected_revision=$4,last_request_deadline=$5,retry_until=GREATEST(retry_until,$5),"
            "updated_at=$6 WHERE project_id=$7 AND owner_id=$8 AND slot_id=$9 "
            "AND scope_workspace_id IS NOT DISTINCT FROM $10::text RETURNING " + std::string(kSlotColumns),
            pqxx::params{tx, Json(merged).dump(), mutation.request_id, mutation.request_hash,
                         static_cast<std::int64_t>(mutation.expected_revision), mutation.request_deadline,
                         now, project, principal.id, slot, scope ? std::optional(scope->workspace_id) : std::nullopt});
        add_committed(response);
        return response;
    });
}

Json Store::list_checkpoints(const Principal& principal, const std::string& project, const std::string& after,
                             std::size_t limit) {
    if (!valid_identifier(project)) invalid_request();
    if (!after.empty() && !valid_slot(after)) invalid_cursor();
    if (limit == 0 || limit > kMaximumListLimit) invalid_limit();
    (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, std::nullopt);
        const auto observed_at = database_now(tx);
        const auto rows = tx.exec(std::string("SELECT ") + std::string(kSlotColumns) +
            " FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2 AND slot_id>$3 "
            "AND scope_workspace_id IS NULL ORDER BY slot_id ASC LIMIT $4",
            pqxx::params{tx, project, principal.id, after, static_cast<std::int64_t>(limit + 1)});
        Json items = Json::array();
        const auto result_limit = static_cast<pqxx::result::size_type>(limit);
        for (pqxx::result::size_type index = 0; index < std::min(result_limit, rows.size()); ++index) {
            auto entry = checkpoint_public_record(record_from_row(rows[index]));
            entry["observed_at"] = observed_at;
            items.push_back(std::move(entry));
        }
        Json next_cursor = nullptr;
        if (rows.size() > result_limit) next_cursor = items.back().at("slot_id");
        return Json{{"schema_version", "shin.task-checkpoint-inventory.v1"}, {"entries", std::move(items)},
                    {"next_cursor", std::move(next_cursor)}};
    });
}

Json Store::prune_checkpoints(const Principal& principal, const std::string& project,
                              const std::optional<WorkspaceReadRequest>& scope) {
    if (!valid_identifier(project)) invalid_request();
    if (!scope) (void)get_project(principal, project);
    return run_transaction([&](pqxx::transaction_base& tx) {
        mutation_scope(tx, principal, project, scope);
        tx.exec("SELECT pg_advisory_xact_lock(hashtext('shin_server.checkpoint-mutations.v1'))");
        const auto now = database_now(tx);
        auto floor = deadline_floor(tx, true);
        const auto removed = tx.exec(
            "DELETE FROM shin_server.checkpoint_slots WHERE project_id=$1 AND owner_id=$2 AND state='cleared' "
            "AND retry_until <= $3 AND scope_workspace_id IS NOT DISTINCT FROM $4::text RETURNING retry_until",
            pqxx::params{tx, project, principal.id, now, scope ? std::optional(scope->workspace_id) : std::nullopt});
        if (static_cast<std::size_t>(removed.size()) > kCheckpointMaximumSlots) corrupt();
        for (const auto& row : removed) {
            const auto retry_until = row["retry_until"].as<std::int64_t>();
            if (retry_until < 0 || retry_until > kMaximumTimestamp) corrupt();
            floor = std::max(floor, retry_until);
        }
        const auto lifecycle = tx.exec(
            "UPDATE shin_server.checkpoint_lifecycle SET reject_deadlines_through=$1 WHERE singleton "
            "RETURNING reject_deadlines_through", pqxx::params{tx, floor});
        if (lifecycle.size() != 1 || lifecycle[0][0].as<std::int64_t>() != floor) corrupt();
        return Json{{"schema_version", "shin.checkpoint-prune.v1"}, {"removed", static_cast<std::int64_t>(removed.size())},
                    {"observed_at", now}, {"read_only", false}};
    });
}

} // namespace shin::server
