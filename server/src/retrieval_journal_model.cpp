#include "shin/server/retrieval_journal.h"
#include "shin/server/config.h"

#include <array>
#include <limits>
#include <vector>

namespace shin::server::journal {
namespace {
[[noreturn]] void invalid() {
    throw ServiceError(500, "journal-invalid-batch", "The native observation batch is invalid.");
}

Json text_field(const Json& object, const char* key, std::size_t max, bool optional = false) {
    const auto& value = object.at(key);
    if (optional && value.is_null()) return value;
    if (!value.is_string()) invalid();
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > max || text.find('\0') != std::string::npos) invalid();
    return value;
}

Json integer_field(const Json& object, const char* key, bool optional = false) {
    const auto& value = object.at(key);
    if (optional && value.is_null()) return value;
    if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::int64_t>::max()) invalid();
    return value;
}

void identifier(const Json& value) {
    if (!value.is_null() && (!value.is_string() || !valid_identifier(value.get<std::string>()))) invalid();
}

void digest(const Json& value) {
    if (value.is_null()) return;
    if (!value.is_string()) invalid();
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != std::string::npos) invalid();
}

void choice(const Json& value, std::initializer_list<const char*> choices, bool optional = false) {
    if (optional && value.is_null()) return;
    for (const auto* allowed : choices) if (value == allowed) return;
    invalid();
}

bool contains_nul(const Json& root) {
    std::vector<const Json*> pending{&root};
    while (!pending.empty()) {
        const auto& value = *pending.back();
        pending.pop_back();
        if (value.is_string()) {
            if (value.get_ref<const std::string&>().find('\0') != std::string::npos) return true;
        } else if (value.is_object()) {
            for (const auto& [key, item] : value.items()) {
                if (key.find('\0') != std::string::npos) return true;
                pending.push_back(&item);
            }
        } else if (value.is_array()) {
            for (const auto& item : value) pending.push_back(&item);
        }
    }
    return false;
}

Json fact(const Json& source, const Json& instance) {
    Json row{{"instance_id", instance}};
    for (const auto* key : {"sequence", "observed_at_unix_ms", "status", "elapsed_ns"})
        row[key] = integer_field(source, key);
    if (row.at("sequence") == 0 || row.at("status") < 100 || row.at("status") > 599) invalid();
    for (const auto* key : {"project_id", "selector_id"}) {
        row[key] = text_field(source, key, 63);
        identifier(row.at(key));
    }
    row["result_bytes"] = integer_field(source, "result_bytes", true);
    for (const auto* key : {"operation", "selector_kind", "last_phase", "query_capture"})
        row[key] = text_field(source, key, 32);
    choice(row.at("operation"), {"search", "brief", "pack"});
    choice(row.at("selector_kind"), {"route", "publication"});
    choice(row.at("query_capture"), {"disabled", "invalid-request", "captured", "omitted-budget", "capture-failed"});
    row["reason_code"] = text_field(source, "reason_code", 64, true);
    if ((row.at("status") == 200) != row.at("reason_code").is_null()) invalid();
    const auto& phases = source.at("phase_ns");
    constexpr std::array<const char*, 5> names{"validation", "selection", "admission", "execution", "finalization"};
    std::uint64_t sum = 0;
    bool ended = false;
    bool matched = false;
    for (const auto* phase : names) {
        const auto value = integer_field(phases, phase, ended);
        if (ended != value.is_null()) invalid();
        row[std::string(phase) + "_ns"] = value;
        if (!value.is_null()) {
            const auto part = value.get<std::uint64_t>();
            if (part > static_cast<std::uint64_t>(INT64_MAX) - sum) invalid();
            sum += part;
        }
        if (row.at("last_phase") == phase) { ended = true; matched = true; }
    }
    if (!matched || sum != row.at("elapsed_ns").get<std::uint64_t>()) invalid();
    const auto& selection = source.at("selection");
    if (row.at("status") != 200 && !selection.is_null()) invalid();
    for (const auto* key : {"route_id", "workspace_id", "source_host_id", "snapshot_id"}) {
        row[key] = selection.is_null() ? Json(nullptr) : text_field(selection, key, 128);
        if (std::string_view(key) != "snapshot_id") identifier(row.at(key));
    }
    for (const auto* key : {"preparation_id", "revision"})
        row[key] = selection.is_null() ? Json(nullptr) : integer_field(selection, key);
    if (source.contains("attribution") && !source.at("attribution").is_null()) {
        const auto& attribution = source.at("attribution");
        if (!attribution.is_object() || attribution.size() != 3 || !attribution.contains("principal_id") ||
            !attribution.contains("tenant_id") || !attribution.contains("workspace_id")) invalid();
        for (const auto* key : {"principal_id", "tenant_id", "workspace_id"}) {
            const auto value = text_field(attribution, key, 63);
            identifier(value);
            row[std::string("scope_") + key] = value;
        }
        if (!row.at("workspace_id").is_null() && row.at("workspace_id") != row.at("scope_workspace_id")) invalid();
    }
    const auto& context = source.at("context");
    for (const auto* key : {"profile_id", "profile_fingerprint", "route_fingerprint"})
        row[key] = context.is_null() ? Json(nullptr) : text_field(context, key, 128);
    if (!row.at("profile_fingerprint").is_null()) {
        const auto profile = row.at("profile_fingerprint").get<std::string>();
        if (profile.size() != 24 || !profile.starts_with("fnv1a64:") ||
            profile.find_first_not_of("0123456789abcdef", 8) != std::string::npos) invalid();
    }
    digest(row.at("route_fingerprint"));
    const auto& quality = source.at("quality");
    row["retrieval_mode"] = text_field(quality, "retrieval_mode", 32, true);
    choice(row.at("retrieval_mode"), {"strict", "relaxed", "owner-scoped", "directory-scoped", "planned", "identifier-scoped"}, true);
    for (const auto* key : {"returned_records", "candidate_rows", "covered_groups", "total_groups",
                          "required_requirements", "satisfied_required_requirements"})
        row[key] = integer_field(quality, key, true);
    for (const auto* key : {"pack_complete", "pack_coverage_complete"}) {
        row[key] = quality.at(key);
        if (!row[key].is_null() && !row[key].is_boolean()) invalid();
    }
    row["request"] = source.at("request");
    row["request_fingerprint"] = nullptr;
    if (row.at("query_capture") == "captured") {
        if (!row.at("request").is_object() || row.at("request").dump().size() > 8192) invalid();
        // JSON permits NUL strings; PostgreSQL JSONB does not. Keep the attempt, not a rewritten body.
        if (contains_nul(row.at("request"))) {
            row["query_capture"] = "capture-failed";
            row["request"] = nullptr;
        } else {
            row["request_fingerprint"] = digest_hex(sha256(Json::array({"shin.exact-request.v1", row.at("project_id"),
                row.at("operation"), row.at("request")}).dump()));
            if (row.contains("scope_principal_id"))
                row["request_fingerprint"] = digest_hex(sha256(Json::array({"shin.scoped-exact-request.v1",
                    row.at("scope_principal_id"), row.at("project_id"), row.at("scope_tenant_id"), row.at("scope_workspace_id"),
                    row.at("operation"), row.at("request")}).dump()));
        }
    } else if (!row.at("request").is_null()) invalid();
    row["record_hash"] = digest_hex(sha256(row.dump()));
    return row;
}
} // namespace

Json normalize_producer(const Json& provenance) {
    try {
        require_fields(provenance, {"instance_id", "source_host_id", "server_version", "core_version"});
        Json output;
        for (const auto* key : {"instance_id", "source_host_id", "server_version", "core_version"})
            output[key] = text_field(provenance, key, 128);
        digest(output.at("instance_id"));
        identifier(output.at("source_host_id"));
        return output;
    } catch (const Json::exception&) { invalid(); }
}

Json normalize_batch(const Json& batch) {
    try {
        Json output;
        for (const auto* key : {"instance_id", "source_host_id", "server_version", "core_version"})
            output[key] = text_field(batch, key, 128);
        digest(output.at("instance_id"));
        identifier(output.at("source_host_id"));
        output["producer_epoch"] = integer_field(batch, "producer_epoch");
        if (output.at("producer_epoch") == 0) invalid();
        output["latest_sequence"] = integer_field(batch, "latest_sequence");
        const auto& records = batch.at("records");
        if (!records.is_array() || records.size() > 256) invalid();
        Json facts = Json::array();
        std::uint64_t previous = 0;
        for (const auto& record : records) {
            auto row = fact(record, output.at("instance_id"));
            // The payload hash stays v1; the registered transport epoch is checked separately.
            row["producer_epoch"] = output.at("producer_epoch");
            const auto sequence = row.at("sequence").get<std::uint64_t>();
            if (sequence <= previous || sequence > output.at("latest_sequence").get<std::uint64_t>()) invalid();
            previous = sequence;
            facts.push_back(std::move(row));
        }
        output["facts"] = std::move(facts);
        return output;
    } catch (const Json::exception&) { invalid(); }
}
} // namespace shin::server::journal
