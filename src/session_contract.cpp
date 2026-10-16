#include "context_internal.h"
#include "session_contract.h"
#include "execution_evidence.h"

namespace shin::context {
namespace {

const Json& session_array(const Json& object, std::string_view field, std::size_t maximum) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_array() || value->size() > maximum) {
        throw_usage_error("invalid-session-array", "Missing, invalid, or oversized session array: " + std::string(field) + ".");
    }
    return *value;
}

std::vector<std::string> session_strings(const Json& object, const std::string& field,
                                        std::size_t count, std::size_t bytes, bool paths = false) {
    std::set<std::string> values;
    for (const Json& item : session_array(object, field, count)) {
        const std::string value = session_string(Json{{"value", item}}, "value", bytes);
        if (!values.insert(value).second || (paths && !profile_relative_path_valid(value))) {
            throw_usage_error("invalid-session-reference", "Session references must be distinct and paths repository-relative.");
        }
    }
    return {values.begin(), values.end()};
}

std::string session_enum(const Json& value, const std::string& field,
                         const std::set<std::string>& allowed) {
    const std::string result = session_string(value, field, 128);
    if (!allowed.contains(result)) {
        throw_usage_error("invalid-session-enum", "Unsupported session value for " + field + ".");
    }
    return result;
}

Json parse_session_state(const Json& state) {
    require_session_fields(state, {"current_slice", "assertions", "changed_paths", "identifiers", "next_actions"});
    Json assertions = Json::array();
    std::set<std::string> ids;
    for (const Json& item : session_array(state, "assertions", 64)) {
        require_session_fields(item, {"id", "kind", "text", "paths", "receipt_ids", "blocker_kind"});
        const std::string id = session_string(item, "id", 128);
        if (!ids.insert(id).second) {
            throw_usage_error("duplicate-session-id", "Assertion ids must be unique.");
        }
        const std::string kind = session_enum(item, "kind", {"decision", "invariant", "completed", "pending", "blocked"});
        Json normalized = {
            {"id", id}, {"kind", kind}, {"text", session_string(item, "text", 2048)},
            {"paths", session_strings(item, "paths", 16, 1024, true)},
            {"receipt_ids", session_strings(item, "receipt_ids", 16, 128)}
        };
        if (kind == "blocked") {
            normalized["blocker_kind"] = session_enum(item, "blocker_kind", {"vpn", "credentials", "user", "external-state"});
        } else if (item.contains("blocker_kind")) {
            throw_usage_error("invalid-session-blocker", "Only blocked assertions may declare blocker_kind.");
        }
        assertions.push_back(std::move(normalized));
    }
    Json actions = Json::array();
    ids.clear();
    for (const Json& item : session_array(state, "next_actions", 16)) {
        require_session_fields(item, {"id", "text"});
        const std::string id = session_string(item, "id", 128);
        if (!ids.insert(id).second) {
            throw_usage_error("duplicate-session-id", "Next action ids must be unique.");
        }
        actions.push_back({{"id", id}, {"text", session_string(item, "text", 1024)}});
    }
    return {
        {"current_slice", session_string(state, "current_slice", 2048)},
        {"assertions", std::move(assertions)},
        {"changed_paths", session_strings(state, "changed_paths", 128, 1024, true)},
        {"identifiers", session_strings(state, "identifiers", 128, 256)},
        {"next_actions", std::move(actions)}
    };
}

Json parse_vcs(const Json& value) {
    if (value.is_null()) {
        return nullptr;
    }
    require_session_fields(value, {"branch", "commit"});
    session_string(value, "branch", 256);
    const std::string commit = session_string(value, "commit", 64);
    if ((commit.size() != 40 && commit.size() != 64) ||
        !std::all_of(commit.begin(), commit.end(), [](unsigned char c) {
            return is_ascii_digit(c) || (c >= 'a' && c <= 'f');
        })) {
        throw_usage_error("invalid-session-commit", "VCS commit must be a full lowercase hex object id.");
    }
    return value;
}

SessionDraft parse_session_payload(const Json& input, const Profile& profile) {
    for (const char* field : {"task_request", "state", "vcs"}) {
        if (!input.contains(field)) {
            throw_usage_error("missing-session-field", "Session input is missing a required payload field.");
        }
    }
    SessionDraft draft;
    draft.request = parse_task_request(input.at("task_request"), profile);
    if (draft.request.objective.size() > 4096 || draft.request.requirements.size() > 12 ||
        draft.request.budget.max_records > 24 || draft.request.budget.max_evidence_bytes > 32768 ||
        draft.request.canonical_document.dump().size() > 16384) {
        throw_usage_error("session-task-too-large", "Continuation requests allow 12 requirements, 24 records, 32 KiB evidence, and 16 KiB request JSON.");
    }
    draft.state = parse_session_state(input.at("state"));
    draft.receipts = parse_execution_receipts(input);
    draft.vcs = parse_vcs(input.at("vcs"));
    draft.ttl_seconds = session_integer(input, "ttl_seconds", 1, kMaxSessionTtl);
    std::set<std::string> receipt_ids;
    for (const Json& receipt : draft.receipts) {
        receipt_ids.insert(receipt.at("receipt_id").get<std::string>());
    }
    for (const Json& assertion : draft.state.at("assertions")) {
        for (const Json& id : assertion.at("receipt_ids")) {
            if (!receipt_ids.contains(id.get<std::string>())) {
                throw_usage_error("unknown-session-receipt", "Assertion references a receipt absent from the checkpoint.");
            }
        }
    }
    return draft;
}

void validate_tracked_sources(const Json& input) {
    std::set<std::string> paths;
    for (const Json& source : session_array(input, "tracked_sources", kMaxSessionTrackedPaths)) {
        require_session_fields(source, {"path", "observation", "content_hash", "bytes"});
        const std::string path = session_string(source, "path", 1024);
        if (!profile_relative_path_valid(path) || !paths.insert(path).second) {
            throw_usage_error("invalid-session-path", "Tracked paths must be distinct repository-relative paths.");
        }
        const std::string observation = session_enum(source, "observation", {"observed", "missing", "unobserved"});
        if (!source.contains("content_hash") || !source.contains("bytes")) {
            throw_usage_error("missing-session-field", "Tracked sources require hash and byte fields.");
        }
        if (observation == "observed") {
            require_session_digest(session_string(source, "content_hash", 80));
            session_integer(source, "bytes", 0, std::numeric_limits<std::int64_t>::max());
        } else if (!source.at("content_hash").is_null() || !source.at("bytes").is_null()) {
            throw_usage_error("invalid-session-source", "Unobserved or missing sources cannot carry content evidence.");
        }
    }
}

}  // namespace

void require_session_fields(const Json& object, const std::set<std::string>& allowed) {
    if (!object.is_object()) {
        throw_usage_error("invalid-session-object", "Session field must be an object.");
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (!allowed.contains(field.key())) {
            throw_usage_error("unknown-session-field", "Session object contains an unknown field. Field content omitted.");
        }
    }
}

std::string session_string(const Json& object, const std::string& field, std::size_t maximum) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) {
        throw_usage_error("invalid-session-string", "Missing or invalid session string: " + field + ".");
    }
    const std::string text = value->get<std::string>();
    require_session_safe_text(text);
    if (trim(text).empty() || text.size() > maximum ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        throw_usage_error("invalid-session-string", "Session strings must be bounded, non-empty, and control-free: " + field + ".");
    }
    return text;
}

std::int64_t session_integer(const Json& object, const std::string& field,
                             std::int64_t minimum, std::int64_t maximum) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) {
        throw_usage_error("invalid-session-integer", "Invalid session integer: " + field + ".");
    }
    const std::int64_t number = value->get<std::int64_t>();
    if (number < minimum || number > maximum) {
        throw_usage_error("invalid-session-integer", "Session integer is outside its bounds: " + field + ".");
    }
    return number;
}

void require_session_digest(const std::string& value) {
    const std::size_t prefix = value.starts_with("fnv1a64:") ? 8 : value.starts_with("sha256:") ? 7 : 0;
    const std::size_t hex_length = prefix == 8 ? 16 : 64;
    if (prefix == 0 || value.size() != prefix + hex_length ||
        !std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix), value.end(), [](unsigned char c) {
            return is_ascii_digit(c) || (c >= 'a' && c <= 'f');
        })) {
        throw_usage_error("invalid-session-fingerprint", "Expected a lowercase fnv1a64 or sha256 fingerprint.");
    }
}

Json parse_session_binding(const Json& value, bool receipt) {
    std::set<std::string> fields = {"root", "profile", "profile_config_hash", "task_id", "thread_id"};
    if (receipt) {
        fields.insert("worktree_id");
        fields.insert("snapshot_id");
    }
    require_session_fields(value, fields);
    for (const std::string& field : fields) {
        const std::string text = session_string(value, field, field == "root" ? 4096 : 128);
        if (field.ends_with("_hash") || field == "worktree_id" || field == "snapshot_id") {
            require_session_digest(text);
        }
        if (field == "root" && (!fs::path(text).is_absolute() || fs::path(text).lexically_normal() != fs::path(text))) {
            throw_usage_error("invalid-session-root", "Checkpoint root must be an absolute canonical path.");
        }
    }
    return value;
}

SessionDraft parse_session_draft(const Json& input, const Profile& profile) {
    require_session_fields(input, {"schema_version", "task_request", "state", "receipts", "vcs", "ttl_seconds"});
    if (session_string(input, "schema_version", 128) != "shin.session-draft.v1") {
        throw_usage_error("unsupported-session-schema", "checkpoint requires shin.session-draft.v1.");
    }
    return parse_session_payload(input, profile);
}

SessionDraft validate_session_checkpoint(const Json& input, const Profile& profile) {
    require_session_fields(input, {"schema_version", "checkpoint_id", "engine_version", "generator_version",
                                   "captured_at", "ttl_seconds", "binding", "baseline", "task_request", "state",
                                   "receipts", "vcs", "tracked_sources", "pack_baseline"});
    if (session_string(input, "schema_version", 128) != "shin.session-checkpoint.v1") {
        throw_usage_error("unsupported-session-schema", "resume requires shin.session-checkpoint.v1.");
    }
    const std::string id = session_string(input, "checkpoint_id", 80);
    Json material = input;
    material.erase("checkpoint_id");
    if (fnv1a64(material.dump()) != id) {
        throw_usage_error("checkpoint-integrity-mismatch", "Checkpoint content differs from its identity.");
    }
    session_string(input, "engine_version", 128);
    session_string(input, "generator_version", 128);
    session_integer(input, "captured_at", 0, kMaxSessionTimestamp);
    if (!input.contains("binding") || !input.contains("baseline") || !input.contains("pack_baseline")) {
        throw_usage_error("missing-session-field", "Checkpoint is missing its baseline or binding.");
    }
    parse_session_binding(input.at("binding"), false);
    const Json& baseline = input.at("baseline");
    require_session_fields(baseline, {"snapshot_id", "worktree_id", "index_current", "observation_complete"});
    for (const char* field : {"snapshot_id", "worktree_id"}) {
        require_session_digest(session_string(baseline, field, 80));
    }
    for (const char* field : {"index_current", "observation_complete"}) {
        if (!baseline.contains(field) || !baseline.at(field).is_boolean()) {
            throw_usage_error("invalid-session-baseline", "Checkpoint baseline requires boolean observation flags.");
        }
    }
    const Json& pack = input.at("pack_baseline");
    if (!pack.is_null()) {
        require_session_fields(pack, {"receipt_id", "request_fingerprint", "evidence_set_fingerprint", "evidence_ids"});
        for (const char* field : {"receipt_id", "request_fingerprint", "evidence_set_fingerprint"}) {
            require_session_digest(session_string(pack, field, 80));
        }
        for (const std::string& evidence : session_strings(pack, "evidence_ids", 24, 80)) {
            require_session_digest(evidence);
        }
    }
    validate_tracked_sources(input);
    return parse_session_payload(input, profile);
}

Json session_binding(const RuntimeOptions& options, const SessionEvaluationOptions& command) {
    return {{"root", options.root.generic_string()}, {"profile", options.profile.id},
            {"profile_config_hash", profile_config_hash(options.profile)},
            {"task_id", command.task_id}, {"thread_id", command.thread_id}};
}

std::string session_age(std::int64_t observed_at, std::int64_t ttl, std::int64_t now) {
    if (now < observed_at) {
        return "future";
    }
    return now - observed_at >= ttl ? "expired" : "current";
}

Json session_policy() {
    return {{"session_authority", "session-asserted"}, {"source_authority_upgraded", false},
            {"read_only", true}, {"actions_executed", false}, {"consumer_mutation", false},
            {"chat_transcript_stored", false}, {"cryptographic_authentication", false},
            {"historical_diff_scope", "tracked-paths-only"}};
}

}  // namespace shin::context
