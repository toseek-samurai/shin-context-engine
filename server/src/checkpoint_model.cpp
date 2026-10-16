#include "shin/server/checkpoint.h"
#include "shin/server/checkpoint_schema.h"
#include "shin/server/config.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

namespace shin::server {
namespace {

bool lowercase_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool session_binding_id(std::string_view value) {
    return value.size() >= 1 && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x21 && character <= 0x7e;
    });
}

bool fnv_id(std::string_view value) {
    return value.size() == 24 && value.starts_with("fnv1a64:") && lowercase_hex(value.substr(8), 16);
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

bool bounded_integer(const Json& value, std::int64_t minimum, std::int64_t maximum) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        return number <= static_cast<std::uint64_t>(maximum) && number >= static_cast<std::uint64_t>(minimum);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

[[noreturn]] void invalid_mutation() {
    throw ServiceError(400, "invalid-request", "Checkpoint mutation identity is invalid.");
}

[[noreturn]] void corrupt_record() {
    throw ServiceError(500, "database-corrupt", "Stored checkpoint data is invalid.");
}

void require_public_field(const Json& record, std::string_view name) {
    if (!record.contains(name)) corrupt_record();
}

void validate_public_record(const Json& record) {
    if (!record.is_object()) corrupt_record();
    constexpr std::array<std::string_view, 14> fields{
        "schema_version", "project_id", "owner_id", "slot_id", "state", "revision", "task_id", "thread_id",
        "provenance", "captured_at", "ttl_seconds", "checkpoint_id", "retry_until", "updated_at"};
    for (const auto field : fields) require_public_field(record, field);

    if (record.at("schema_version") != "shin.task-checkpoint.v1" ||
        !record.at("project_id").is_string() || !valid_identifier(record.at("project_id").get<std::string>()) ||
        !record.at("owner_id").is_string() || !valid_identifier(record.at("owner_id").get<std::string>()) ||
        !record.at("slot_id").is_string() || !valid_identifier(record.at("slot_id").get<std::string>()) ||
        !record.at("state").is_string() ||
        (record.at("state") != "active" && record.at("state") != "cleared") ||
        !bounded_integer(record.at("revision"), 1, std::numeric_limits<std::int64_t>::max()) ||
        !record.at("task_id").is_string() || !session_binding_id(record.at("task_id").get<std::string>()) ||
        !record.at("thread_id").is_string() || !session_binding_id(record.at("thread_id").get<std::string>()) ||
        !checkpoint_provenance_valid(record.at("provenance"), record.at("project_id").get<std::string>()) ||
        !bounded_integer(record.at("retry_until"), 0, 253402300799) ||
        !bounded_integer(record.at("updated_at"), 0, 253402300799)) {
        corrupt_record();
    }

    const bool active = record.at("state") == "active";
    if (active) {
        if (!bounded_integer(record.at("captured_at"), 0, 253402300799) ||
            !bounded_integer(record.at("ttl_seconds"), 1, 604800) || !record.at("checkpoint_id").is_string() ||
            !fnv_id(record.at("checkpoint_id").get<std::string>())) {
            corrupt_record();
        }
    } else if (!record.at("captured_at").is_null() || !record.at("ttl_seconds").is_null() ||
               !record.at("checkpoint_id").is_null()) {
        corrupt_record();
    }
}

} // namespace

bool checkpoint_provenance_valid(const Json& provenance, std::string_view project) {
    try {
        if (!provenance.is_object() || provenance.size() != 9 || provenance.dump().size() > 4096 || !no_nul(provenance)) {
            return false;
        }
        for (const auto* field : {"project_id", "workspace_id", "source_host_id", "route_id", "profile_id"}) {
            const auto& value = provenance.at(field);
            if (!value.is_string() || !valid_identifier(value.get<std::string>())) return false;
        }
        if (provenance.at("project_id") != project || !provenance.at("snapshot_id").is_string() ||
            !fnv_id(provenance.at("snapshot_id").get<std::string>())) {
            return false;
        }
        const auto& selector = provenance.at("selector");
        if (!selector.is_object() || selector.size() != 2 || !selector.at("kind").is_string() || !selector.at("id").is_string() ||
            (selector.at("kind") != "route" && selector.at("kind") != "publication") ||
            !valid_identifier(selector.at("id").get<std::string>())) {
            return false;
        }
        if (!provenance.at("preparation_id").is_null() &&
            !bounded_integer(provenance.at("preparation_id"), 0, std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        return bounded_integer(provenance.at("revision"), 0, std::numeric_limits<std::int64_t>::max());
    } catch (const Json::exception&) {
        return false;
    }
}

bool checkpoint_receipt_origins_valid(const Json& checkpoint, const std::string& owner, const Json& origins) {
    if (!origins.is_array() || origins.size() > 32) return false;
    if (origins.empty()) return true;
    try {
        const auto& receipts = checkpoint.at("receipts");
        const auto& binding = checkpoint.at("binding");
        if (!receipts.is_array() || receipts.size() > 32) return false;
        std::set<std::string> eligible;
        for (const auto& receipt : receipts) {
            if (receipt.at("producer") != owner) continue;
            bool same_scope = true;
            for (const auto* field : {"root", "profile", "profile_config_hash", "task_id", "thread_id"}) {
                if (receipt.at("scope").at(field) != binding.at(field)) same_scope = false;
            }
            if (same_scope) eligible.insert(digest_hex(sha256(receipt.dump())));
        }
        std::string previous;
        for (const auto& origin : origins) {
            if (!origin.is_string()) return false;
            const auto& digest = origin.get_ref<const std::string&>();
            if (!lowercase_hex(digest, 64) || digest <= previous || !eligible.contains(digest)) return false;
            previous = digest;
        }
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

void validate_checkpoint_mutation(const CheckpointMutation& mutation) {
    if (!session_binding_id(mutation.task_id) || !session_binding_id(mutation.thread_id) ||
        !lowercase_hex(mutation.request_id, 32) || !lowercase_hex(mutation.request_hash, 64) ||
        mutation.expected_revision > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        mutation.request_deadline < 0 || mutation.request_deadline > 253402300799) {
        invalid_mutation();
    }
}

Json checkpoint_public_record(const Json& record) {
    validate_public_record(record);
    Json result = Json::object();
    for (const auto* field : {"schema_version", "project_id", "owner_id", "slot_id", "state", "revision", "task_id",
                              "thread_id", "provenance", "captured_at", "ttl_seconds", "checkpoint_id", "retry_until", "updated_at"}) {
        result[field] = record.at(field);
    }
    const auto& provenance = record.at("provenance");
    result["provenance"] = {{"project_id", provenance.at("project_id")}, {"workspace_id", provenance.at("workspace_id")},
                            {"source_host_id", provenance.at("source_host_id")}, {"route_id", provenance.at("route_id")},
                            {"profile_id", provenance.at("profile_id")},
                            {"selector", {{"kind", provenance.at("selector").at("kind")}, {"id", provenance.at("selector").at("id")}}},
                            {"snapshot_id", provenance.at("snapshot_id")}, {"preparation_id", provenance.at("preparation_id")},
                            {"revision", provenance.at("revision")}};
    return result;
}

} // namespace shin::server
