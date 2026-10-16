#include "shin/observer/outbox.h"
#include "shin/observer_protocol.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace shin::observer {
namespace {
[[noreturn]] void invalid() { throw Error("invalid-outbox", "Stored outbox is inconsistent; preserve state for review."); }

void fields(const Json& value, const std::set<std::string>& keys) {
    if (!value.is_object() || value.size() != keys.size() ||
        !std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return value.contains(key); })) invalid();
}

std::uint64_t counter(const Json& value) {
    if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::int64_t>::max() - 1) invalid();
    return value.get<std::uint64_t>();
}

bool hex(const Json& value, std::size_t length) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

Json comparable(Json observation) {
    for (const auto* key : {"started_at_ms", "observed_at_ms", "duration_ms"}) observation.erase(key);
    return observation;
}

std::size_t capacity(const Config& config) {
    // Exact binding size plus fixed reserve for bounded schemas, counters and latest observation.
    constexpr std::size_t metadata_reserve = 8192;
    const auto binding_bytes = config.binding.dump().size();
    if (binding_bytes >= kMaximumStateBytes - metadata_reserve) invalid();
    return std::min<std::uint64_t>(config.outbox_max_bytes, kMaximumStateBytes - metadata_reserve - binding_bytes);
}

Json producer_binding(const Config& config) {
    Json result;
    for (const auto* key : {"observer_id", "source_host_id", "project_id", "workspace_id"}) result[key] = config.binding.at(key);
    result["profile"] = config.profile;
    result["workspace_binding_digest"] = config.binding_key;
    return result;
}

Json submission(const Config& config, const Json& state, const Json& outbox) {
    const auto sequence = counter(outbox.at("accepted_sequence")) + 1;
    auto payload = producer_binding(config);
    payload["observer_epoch"] = state.at("epoch");
    payload["observer_sequence"] = state.at("sequence");
    payload["observation"] = state.at("observation");
    payload["verification_level"] = "producer-observed";
    payload["shared_publication_mutated"] = false;
    const auto stream = outbox.at("stream_id").get<std::string>();
    return {{"schema_version", "shin.observer-submission.v1"}, {"stream_id", stream}, {"sequence", sequence},
        {"submission_id", digest(stream + ":" + std::to_string(sequence))},
        {"payload_sha256", digest(payload.dump())}, {"payload", std::move(payload)}};
}
} // namespace

Json new_outbox(const Config& config, const std::string& stream_id) {
    if (!config.outbox_enabled) return nullptr;
    if (!hex(stream_id, 32)) invalid();
    return {{"schema_version", "shin.observer-outbox.v2"}, {"stream_id", stream_id},
        {"accepted_sequence", 0}, {"acknowledged_sequence", 0}, {"acknowledged_observation_sequence", 0},
        {"last_receipt", nullptr}, {"destination", nullptr},
        {"delivery", {{"attempts", 0}, {"failures", 0}, {"last_outcome", "never-attempted"},
            {"last_attempt_at_ms", nullptr}, {"last_result_at_ms", nullptr}, {"last_duration_ms", 0.0}}},
        {"entries", Json::array()}, {"payload_bytes", 0}, {"coalesced_observations", 0},
        {"rejected_observations", 0}, {"first_rejected_observation_sequence", nullptr},
        {"last_rejected_observation_sequence", nullptr}};
}

void capture_outbox(const Config& config, Json& state) {
    if (!config.outbox_enabled) return;
    auto& queue = state.at("outbox");
    auto& entries = queue.at("entries");
    if (!entries.empty() && comparable(entries.back().at("payload").at("observation")) == comparable(state.at("observation"))) {
        queue["coalesced_observations"] = counter(queue.at("coalesced_observations")) + 1;
        return;
    }
    auto entry = submission(config, state, queue);
    const auto bytes = entry.dump().size();
    const auto used = counter(queue.at("payload_bytes"));
    const auto limit = capacity(config);
    if (entries.size() >= config.outbox_max_entries || used > limit || bytes > limit - used) {
        queue["rejected_observations"] = counter(queue.at("rejected_observations")) + 1;
        if (queue.at("first_rejected_observation_sequence").is_null()) queue["first_rejected_observation_sequence"] = state.at("sequence");
        queue["last_rejected_observation_sequence"] = state.at("sequence");
        return;
    }
    entries.push_back(std::move(entry));
    queue["accepted_sequence"] = counter(queue.at("accepted_sequence")) + 1;
    queue["payload_bytes"] = used + bytes;
}

void verify_outbox(const Config& config, const Json& state) {
    const auto& queue = state.at("outbox");
    if (!config.outbox_enabled) {
        if (!queue.is_null()) invalid();
        return;
    }
    fields(queue, {"schema_version", "stream_id", "entries", "payload_bytes", "coalesced_observations",
        "rejected_observations", "first_rejected_observation_sequence", "last_rejected_observation_sequence",
        "accepted_sequence", "acknowledged_sequence", "acknowledged_observation_sequence", "last_receipt", "destination", "delivery"});
    if (queue.at("schema_version") != "shin.observer-outbox.v2" || !hex(queue.at("stream_id"), 32) ||
        !queue.at("entries").is_array() || queue.at("entries").size() > config.outbox_max_entries) invalid();
    const auto observed = counter(state.at("sequence"));
    const auto accepted = counter(queue.at("accepted_sequence"));
    const auto acknowledged = counter(queue.at("acknowledged_sequence"));
    const auto ack_observed = counter(queue.at("acknowledged_observation_sequence"));
    if (acknowledged > accepted || queue.at("entries").size() != accepted - acknowledged ||
        ack_observed > observed || ack_observed < acknowledged) invalid();
    const auto& destination = queue.at("destination");
    if (!destination.is_null()) {
        fields(destination, {"host", "port", "server_id", "principal_id"});
        if (destination.at("host") != "127.0.0.1" || !hex(destination.at("server_id"), 32) ||
            counter(destination.at("port")) < 1024 || counter(destination.at("port")) > 65535 ||
            !destination.at("principal_id").is_string()) invalid();
        const auto& id = destination.at("principal_id").get_ref<const std::string&>();
        if (id.empty() || id.size() > 63 || id.front() < 'a' || id.front() > 'z' ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) invalid();
    }
    const auto& receipt = queue.at("last_receipt");
    if (acknowledged == 0) {
        if (!receipt.is_null() || ack_observed != 0) invalid();
    } else {
        if (destination.is_null()) invalid();
        try { observer_protocol::verify_acknowledgement_shape(receipt); }
        catch (const observer_protocol::ValidationError&) { invalid(); }
        if (receipt.at("server_id") != destination.at("server_id") || receipt.at("stream_id") != queue.at("stream_id") ||
            receipt.at("sequence") != acknowledged || receipt.at("project_id") != config.binding.at("project_id") ||
            receipt.at("workspace_id") != config.binding.at("workspace_id") ||
            receipt.at("submission_id") != digest(queue.at("stream_id").get<std::string>() + ":" + std::to_string(acknowledged))) invalid();
    }
    const auto& delivery = queue.at("delivery");
    fields(delivery, {"attempts", "failures", "last_outcome", "last_attempt_at_ms", "last_result_at_ms", "last_duration_ms"});
    const auto attempts = counter(delivery.at("attempts"));
    const auto failures = counter(delivery.at("failures"));
    if (failures > attempts || acknowledged > attempts - failures ||
        (destination.is_null() && attempts != 0) || !delivery.at("last_outcome").is_string() ||
        !delivery.at("last_duration_ms").is_number() || !std::isfinite(delivery.at("last_duration_ms").get<double>()) ||
        delivery.at("last_duration_ms") < 0) invalid();
    const std::set<std::string> outcomes{"never-attempted", "in-flight", "acknowledged", "transport-unavailable", "server-unavailable",
        "authorization-denied", "identity-mismatch", "protocol-invalid", "server-rejected", "worker-failed", "history-retired"};
    if (!outcomes.contains(delivery.at("last_outcome").get<std::string>())) invalid();
    if ((delivery.at("last_outcome") == "in-flight" && queue.at("entries").empty()) ||
        (delivery.at("last_outcome") == "acknowledged" && acknowledged == 0)) invalid();
    if (attempts == 0) {
        if (delivery.at("last_outcome") != "never-attempted" || !delivery.at("last_attempt_at_ms").is_null() ||
            !delivery.at("last_result_at_ms").is_null() || delivery.at("last_duration_ms") != 0) invalid();
    } else {
        (void)counter(delivery.at("last_attempt_at_ms"));
        if (delivery.at("last_outcome") == "never-attempted") invalid();
        if (!delivery.at("last_result_at_ms").is_null()) (void)counter(delivery.at("last_result_at_ms"));
        if (delivery.at("last_outcome") != "in-flight" && delivery.at("last_result_at_ms").is_null()) invalid();
    }
    const auto coalesced = counter(queue.at("coalesced_observations"));
    const auto rejected = counter(queue.at("rejected_observations"));
    // Subtraction avoids wrapping hostile persisted counter sums.
    if (accepted > observed || coalesced > observed - accepted || rejected != observed - accepted - coalesced) invalid();
    const auto& first = queue.at("first_rejected_observation_sequence");
    const auto& last = queue.at("last_rejected_observation_sequence");
    if (rejected == 0) {
        if (!first.is_null() || !last.is_null()) invalid();
    } else if (counter(first) == 0 || counter(first) > counter(last) || counter(last) > observed ||
               rejected > counter(last) - counter(first) + 1) invalid();
    const auto stream = queue.at("stream_id").get<std::string>();
    const auto binding = producer_binding(config);
    const auto limit = capacity(config);
    std::uint64_t previous_observation = ack_observed;
    std::uint64_t accepted_in_gap_range = 0;
    std::uint64_t sequence = acknowledged;
    std::size_t bytes = 0;
    for (const auto& entry : queue.at("entries")) {
        try { shin::observer_protocol::verify_submission(entry); }
        catch (const shin::observer_protocol::ValidationError&) { invalid(); }
        ++sequence;
        const auto& payload = entry.at("payload");
        if (entry.at("stream_id") != stream || counter(entry.at("sequence")) != sequence) invalid();
        for (auto it = binding.begin(); it != binding.end(); ++it) if (payload.at(it.key()) != it.value()) invalid();
        const auto capture = counter(payload.at("observer_sequence"));
        if (capture <= previous_observation || capture > observed || payload.at("observation").is_null()) invalid();
        if (rejected != 0) {
            if (capture == counter(first) || capture == counter(last)) invalid();
            if (capture > counter(first) && capture < counter(last)) ++accepted_in_gap_range;
        }
        previous_observation = capture;
        const auto size = entry.dump().size();
        if (size > limit - bytes) invalid();
        bytes += size;
    }
    if (counter(queue.at("payload_bytes")) != bytes) invalid();
    if (accepted == 0 && coalesced != 0) invalid();
    if (rejected != 0 && rejected > counter(last) - counter(first) + 1 - accepted_in_gap_range) invalid();
}

void acknowledge_outbox(const Config& config, Json& state, const Json& receipt) {
    auto& queue = state.at("outbox");
    auto& entries = queue.at("entries");
    if (entries.empty() || queue.at("destination").is_null()) invalid();
    try { observer_protocol::verify_acknowledgement(receipt, entries.front(), queue.at("destination").at("server_id").get<std::string>()); }
    catch (const observer_protocol::ValidationError&) { invalid(); }
    queue["acknowledged_sequence"] = entries.front().at("sequence");
    queue["acknowledged_observation_sequence"] = entries.front().at("payload").at("observer_sequence");
    queue["last_receipt"] = receipt;
    queue["payload_bytes"] = counter(queue.at("payload_bytes")) - entries.front().dump().size();
    entries.erase(entries.begin());
    verify_outbox(config, state);
}

Json outbox_summary(const Config& config, const Json& queue) {
    if (queue.is_null()) return {{"enabled", false}, {"delivery", "disabled"}};
    const auto& entries = queue.at("entries");
    return {{"enabled", true}, {"stream_id", queue.at("stream_id")}, {"pending_entries", entries.size()},
        {"pending_payload_bytes", queue.at("payload_bytes")}, {"max_entries", config.outbox_max_entries},
        {"max_payload_bytes", config.outbox_max_bytes}, {"coalesced_observations", queue.at("coalesced_observations")},
        {"effective_max_payload_bytes", capacity(config)},
        {"capacity_rejections", queue.at("rejected_observations")}, {"resync_required", queue.at("rejected_observations") != 0},
        {"first_rejected_observation_sequence", queue.at("first_rejected_observation_sequence")},
        {"last_rejected_observation_sequence", queue.at("last_rejected_observation_sequence")},
        {"oldest_capture_at_ms", entries.empty() ? Json(nullptr) : entries.front().at("payload").at("observation").at("observed_at_ms")},
        {"accepted_sequence", queue.at("accepted_sequence")}, {"acknowledged_sequence", queue.at("acknowledged_sequence")},
        {"server_acknowledged", queue.at("acknowledged_sequence") != 0},
        {"pending_drained", entries.empty()}, {"delivery", queue.at("delivery")}, {"destination", queue.at("destination")}};
}
} // namespace shin::observer
