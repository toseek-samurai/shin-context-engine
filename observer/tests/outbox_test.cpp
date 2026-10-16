#include "shin/observer/outbox.h"
#include "shin/observer/metrics.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace shin::observer;

constexpr const char* kStreamId = "0123456789abcdef0123456789abcdef";
constexpr const char* kEpoch = "fedcba9876543210fedcba9876543210";
constexpr const char* kBindingKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kFingerprint = "fnv1a64:0123456789abcdef";
const std::string kForeignBindingKey(64, 'f');

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(const char* code, Action action) {
    try {
        action();
    } catch (const Error& error) {
        check(error.code == code, "unexpected observer error code");
        return;
    }
    throw std::runtime_error("invalid state was accepted");
}

Config config(bool enabled = true, std::uint64_t max_entries = 8, std::uint64_t max_bytes = 16 * 1024) {
    Config result;
    result.binding = {{"observer_id", "observer-test-agent"}, {"source_host_id", "observer-test-host"},
        {"project_id", "observer-test-project"}, {"workspace_id", "observer-test-workspace"}};
    result.binding_key = kBindingKey;
    result.profile = "observer-fixture";
    result.outbox_enabled = enabled;
    result.outbox_max_entries = max_entries;
    result.outbox_max_bytes = max_bytes;
    return result;
}

Json source_counts(std::uint64_t documents = 0, std::uint64_t source_facts = 0, std::uint64_t capped_files = 0) {
    return {{"documents", documents}, {"source_facts", source_facts}, {"candidate_files", 0},
        {"capped_files", capped_files}, {"skipped_files", 0}};
}

Json changes(std::uint64_t total) {
    return {{"metadata_changed", 0}, {"documents_added", 0}, {"documents_removed", 0},
        {"documents_changed", total}, {"source_facts_added", 0}, {"source_facts_removed", 0},
        {"source_facts_changed", 0}, {"total_changes", total}};
}

Json observation(const std::string& status = "unpublished", std::uint64_t source_variant = 0,
                 std::uint64_t started_at_ms = 100, std::uint64_t observed_at_ms = 110,
                 double duration_ms = 10.0) {
    Json result{{"status", status}, {"reason_code", "index-unavailable"}, {"started_at_ms", started_at_ms},
        {"snapshot_id", nullptr}, {"worktree_id", kFingerprint}, {"snapshot_current", nullptr},
        {"profile_config_hash", kFingerprint}, {"source_counts", source_counts(source_variant)}, {"changes", nullptr},
        {"details_omitted", 0}, {"policy", "periodic-profile-content-comparison"},
        {"verification_level", "local-source-observed"}, {"source_mutated", false}, {"index_mutated", false},
        {"git_required", false}, {"readiness_assessed", false}, {"observed_at_ms", observed_at_ms},
        {"duration_ms", duration_ms}};
    if (status == "current") {
        result["reason_code"] = nullptr;
        result["snapshot_id"] = kFingerprint;
        result["snapshot_current"] = true;
        result["changes"] = changes(0);
    } else if (status == "stale") {
        result["reason_code"] = "worktree-not-current";
        result["snapshot_id"] = kFingerprint;
        result["snapshot_current"] = false;
        result["changes"] = changes(1);
        result["details_omitted"] = 1;
    } else if (status == "incomplete") {
        result["reason_code"] = "incomplete-source-observation";
        result["source_counts"] = source_counts(0, 1, 1);
    } else if (status == "error") {
        result["reason_code"] = "probe-failed";
        result["snapshot_id"] = nullptr;
        result["worktree_id"] = nullptr;
        result["snapshot_current"] = nullptr;
        result["profile_config_hash"] = nullptr;
        result["source_counts"] = nullptr;
        result["changes"] = nullptr;
    }
    verify_observation(result);
    return result;
}

Json state(const Config& active) {
    return {{"binding_key", active.binding_key}, {"binding", active.binding}, {"epoch", kEpoch}, {"sequence", 0},
        {"observation", nullptr}, {"outbox", new_outbox(active, kStreamId)}, {"metrics", new_metrics()}};
}

void capture(Json& value, const Config& active, Json sample) {
    value["observation"] = std::move(sample);
    value["sequence"] = value.at("sequence").get<std::uint64_t>() + 1;
    record_metrics(value.at("metrics"), value.at("observation"));
    capture_outbox(active, value);
}

void verify(const Config& active, const Json& value) {
    verify_outbox(active, value);
    verify_metrics(value);
}

void disabled_outbox_stays_null() {
    const auto active = config(false);
    auto value = state(active);
    check(value.at("outbox").is_null(), "disabled outbox must be null");
    rejects("invalid-outbox", [&] { (void)new_outbox(config(), "not-a-stream-id"); });

    capture(value, active, observation());
    verify(active, value);
    auto unexpected_queue = value;
    unexpected_queue["outbox"] = new_outbox(config(), kStreamId);
    rejects("invalid-outbox", [&] { verify_outbox(active, unexpected_queue); });
    const auto summary = outbox_summary(active, value.at("outbox"));
    check(summary == Json{{"enabled", false}, {"delivery", "disabled"}}, "disabled outbox summary changed");
}

void appends_and_coalesces_without_mutating_entries() {
    const auto active = config();
    auto value = state(active);
    check(value.at("outbox") == Json{{"schema_version", "shin.observer-outbox.v2"}, {"stream_id", kStreamId},
              {"accepted_sequence", 0}, {"acknowledged_sequence", 0}, {"acknowledged_observation_sequence", 0},
              {"last_receipt", nullptr}, {"destination", nullptr},
              {"delivery", {{"attempts", 0}, {"failures", 0}, {"last_outcome", "never-attempted"},
                  {"last_attempt_at_ms", nullptr}, {"last_result_at_ms", nullptr}, {"last_duration_ms", 0.0}}},
              {"entries", Json::array()}, {"payload_bytes", 0}, {"coalesced_observations", 0},
              {"rejected_observations", 0}, {"first_rejected_observation_sequence", nullptr},
              {"last_rejected_observation_sequence", nullptr}},
          "new outbox schema is invalid");
    capture(value, active, observation());
    const auto first_entry = value.at("outbox").at("entries").at(0);
    const auto& payload = first_entry.at("payload");
    check(value.at("outbox").at("entries").size() == 1, "first observation was not queued");
    check(first_entry.at("sequence") == 1 && first_entry.at("submission_id") == digest(std::string(kStreamId) + ":1"),
          "first submission identity is invalid");
    check(first_entry.at("payload_sha256") == digest(payload.dump()) && payload.at("observer_sequence") == 1,
          "first submission digest is invalid");
    check(value.at("outbox").at("payload_bytes") == static_cast<std::uint64_t>(first_entry.dump().size()),
          "outbox did not count the compact serialized entry envelope");
    check(payload.at("observer_id") == active.binding.at("observer_id") &&
              payload.at("workspace_binding_digest") == active.binding_key,
          "first submission binding is invalid");

    capture(value, active, observation("unpublished", 0, 200, 220, 20.0));
    check(value.at("outbox").at("entries").size() == 1 && value.at("outbox").at("entries").at(0) == first_entry,
          "coalescing mutated the retained submission");
    check(value.at("outbox").at("entries").at(0).at("payload").at("observation").at("started_at_ms") == 100 &&
              value.at("outbox").at("entries").at(0).at("payload").at("observation").at("observed_at_ms") == 110 &&
              value.at("outbox").at("entries").at(0).at("payload").at("observation").at("duration_ms") == 10.0,
          "coalescing rewrote retained observation timestamps");
    check(value.at("outbox").at("coalesced_observations") == 1, "unchanged observation was not coalesced");
    verify(active, value);

    capture(value, active, observation("unpublished", 1, 300, 330, 30.0));
    check(value.at("outbox").at("entries").size() == 2, "source change was not appended");
    check(value.at("outbox").at("entries").at(1).at("sequence") == 2 &&
              value.at("outbox").at("entries").at(1).at("payload").at("observer_sequence") == 3,
          "queued and observer sequences diverged");
    verify(active, value);
}

void capacity_rejection_preserves_fifo_and_requires_resync() {
    const auto active = config(true, 2);
    auto value = state(active);
    capture(value, active, observation("unpublished", 0));
    capture(value, active, observation("unpublished", 1));
    const auto retained = value.at("outbox").at("entries");
    capture(value, active, observation("unpublished", 2));

    const auto& queue = value.at("outbox");
    check(queue.at("entries") == retained && queue.at("rejected_observations") == 1,
          "capacity rejection changed FIFO entries");
    check(queue.at("first_rejected_observation_sequence") == 3 && queue.at("last_rejected_observation_sequence") == 3,
          "capacity rejection sequence range is invalid");
    capture(value, active, observation("unpublished", 1, 400, 450, 50.0));
    check(queue.at("rejected_observations") == 1 && queue.at("coalesced_observations") == 1 &&
              queue.at("first_rejected_observation_sequence") == 3 && queue.at("last_rejected_observation_sequence") == 3,
          "resync state was not sticky after a later coalesced observation");
    const auto summary = outbox_summary(active, queue);
    check(summary.at("pending_entries") == 2 && summary.at("capacity_rejections") == 1 &&
              summary.at("resync_required") == true,
          "capacity summary did not retain sticky resync state");
    verify(active, value);
}

void byte_capacity_rejects_before_entry_capacity() {
    const auto active = config(true, 128, 4096);
    auto value = state(active);
    for (std::uint64_t variant = 0; variant != 32 && value.at("outbox").at("rejected_observations") == 0; ++variant)
        capture(value, active, observation("unpublished", variant));

    const auto& queue = value.at("outbox");
    check(queue.at("rejected_observations") != 0, "byte capacity did not reject an entry");
    check(queue.at("entries").size() < active.outbox_max_entries &&
              queue.at("payload_bytes").get<std::uint64_t>() <= active.outbox_max_bytes,
          "byte capacity exceeded its configured limit");
    verify(active, value);
}

Json populated_state() {
    const auto active = config();
    auto value = state(active);
    capture(value, active, observation("unpublished", 0));
    capture(value, active, observation("unpublished", 1));
    return value;
}

void verifier_rejects_tampering() {
    const auto active = config();
    const auto valid = populated_state();
    verify(active, valid);

    auto malformed = valid;
    malformed["outbox"].erase("entries");
    rejects("invalid-outbox", [&] { verify_outbox(active, malformed); });

    auto bad_digest = valid;
    bad_digest["outbox"]["entries"][0]["payload_sha256"] = kBindingKey;
    rejects("invalid-outbox", [&] { verify_outbox(active, bad_digest); });

    auto bad_binding = valid;
    auto& bad_payload = bad_binding["outbox"]["entries"][0]["payload"];
    bad_payload["workspace_binding_digest"] = kForeignBindingKey;
    bad_binding["outbox"]["entries"][0]["payload_sha256"] = digest(bad_payload.dump());
    rejects("invalid-outbox", [&] { verify_outbox(active, bad_binding); });

    auto bad_order = valid;
    std::swap(bad_order["outbox"]["entries"][0], bad_order["outbox"]["entries"][1]);
    rejects("invalid-outbox", [&] { verify_outbox(active, bad_order); });

    auto bad_observation_order = valid;
    auto& out_of_order_payload = bad_observation_order["outbox"]["entries"][1]["payload"];
    out_of_order_payload["observer_sequence"] = 1;
    bad_observation_order["outbox"]["entries"][1]["payload_sha256"] = digest(out_of_order_payload.dump());
    rejects("invalid-outbox", [&] { verify_outbox(active, bad_observation_order); });

    auto bad_counter_sum = valid;
    bad_counter_sum["outbox"]["coalesced_observations"] = 1;
    rejects("invalid-outbox", [&] { verify_outbox(active, bad_counter_sum); });
}

void serialization_preserves_restart_state() {
    const auto active = config();
    auto value = populated_state();
    capture(value, active, observation("unpublished", 1, 400, 440, 40.0));
    const auto restarted = parse_json(value.dump());
    check(restarted == value, "state JSON did not round-trip identically");
    verify(active, restarted);
}

void reserved_state_budget_and_rejection_ranges() {
    auto active = config(true, 128, 192 * 1024);
    active.binding["root"] = std::string(18000, '\x01');
    auto value = state(active);
    for (std::uint64_t index = 0; index < 128; ++index) capture(value, active, observation("unpublished", index));
    verify(active, value);
    const auto summary = outbox_summary(active, value.at("outbox"));
    check(summary.at("effective_max_payload_bytes") < active.outbox_max_bytes && summary.at("capacity_rejections") != 0,
          "escaped binding did not reserve bounded state overhead");
    check(value.dump().size() < kMaximumStateBytes, "full committed state exceeded its atomic byte budget");

    const auto small = config(true, 2);
    auto impossible = state(small);
    capture(impossible, small, observation("unpublished", 0));
    capture(impossible, small, observation("unpublished", 1));
    capture(impossible, small, observation("unpublished", 2));
    impossible["outbox"]["first_rejected_observation_sequence"] = 1;
    rejects("invalid-outbox", [&] { verify_outbox(small, impossible); });
    impossible["outbox"]["first_rejected_observation_sequence"] = 2;
    impossible["outbox"]["last_rejected_observation_sequence"] = 2;
    rejects("invalid-outbox", [&] { verify_outbox(small, impossible); });
}

void metrics_track_retained_samples() {
    auto metrics = new_metrics();
    const std::pair<const char*, double> samples[] = {
        {"current", 1.0}, {"stale", 2.0}, {"unpublished", 3.0}, {"incomplete", 4.0}, {"error", 5.0},
    };
    for (const auto& [status, duration] : samples) record_metrics(metrics, observation(status, 0, 100, 110, duration));
    check(metrics.at("schema_version") == "shin.observer-metrics.v1" &&
              metrics.at("scope") == "retained-committed-samples" && metrics.at("sample_count") == 5,
          "new metrics schema or sample count is invalid");
    for (const auto& [status, duration] : samples) {
        (void)duration;
        check(metrics.at("outcomes").at(status) == 1, "metrics outcome was not recorded");
    }
    check(metrics.at("scan_duration_ms") == Json{{"last", 5.0}, {"max", 5.0}, {"total", 15.0}},
          "metrics durations are invalid");

    Json value{{"sequence", 5}, {"observation", observation("error", 0, 100, 110, 5.0)}, {"metrics", metrics}};
    verify_metrics(value);
    value["metrics"]["outcomes"]["error"] = 0;
    rejects("invalid-metrics", [&] { verify_metrics(value); });
}

Json receipt_for(const Json& entry) {
    return {{"schema_version", "shin.observer-ack.v1"}, {"server_id", kEpoch}, {"stream_id", kStreamId},
        {"sequence", entry.at("sequence")}, {"submission_id", entry.at("submission_id")},
        {"payload_sha256", entry.at("payload_sha256")}, {"project_id", "observer-test-project"},
        {"workspace_id", "observer-test-workspace"}, {"received_at", "2026-09-06T12:00:00.123456Z"},
        {"verification_level", "producer-observed"}, {"server_source_verified", false}, {"shared_publication_mutated", false}};
}

void committed_receipts_drain_without_reusing_sequence() {
    const auto active = config(true, 2);
    auto value = populated_state();
    capture(value, active, observation("unpublished", 2));
    auto& queue = value.at("outbox");
    queue["destination"] = {{"host", "127.0.0.1"}, {"port", 19125}, {"server_id", kEpoch}, {"principal_id", "reader"}};
    auto& delivery = queue.at("delivery");
    delivery["attempts"] = 1;
    delivery["last_attempt_at_ms"] = 100;
    delivery["last_outcome"] = "in-flight";
    const auto first = queue.at("entries").front();
    const auto second = queue.at("entries").back();
    auto wrong = receipt_for(first);
    wrong["payload_sha256"] = kForeignBindingKey;
    const auto unchanged = value;
    rejects("invalid-outbox", [&] { acknowledge_outbox(active, value, wrong); });
    check(value == unchanged, "invalid receipt modified the queue");
    delivery["last_outcome"] = "acknowledged";
    delivery["last_result_at_ms"] = 110;
    acknowledge_outbox(active, value, receipt_for(first));
    check(queue.at("acknowledged_sequence") == 1 && queue.at("entries") == Json::array({second}), "receipt removed the wrong entry");
    delivery["attempts"] = 2;
    acknowledge_outbox(active, value, receipt_for(second));
    check(queue.at("entries").empty() && queue.at("payload_bytes") == 0 && queue.at("accepted_sequence") == 2,
          "draining lost the issued cursor or retained bytes");
    capture(value, active, observation("unpublished", 3));
    check(queue.at("entries").front().at("sequence") == 3 && queue.at("entries").front().at("payload").at("observer_sequence") == 4,
          "new entry reused an acknowledged sequence or lost the capture gap");
    check(outbox_summary(active, queue).at("resync_required") == true, "acknowledgement cleared the sticky lost interval");
    verify(active, value);
    auto invalid_cursor = value;
    invalid_cursor["outbox"]["acknowledged_sequence"] = 3;
    rejects("invalid-outbox", [&] { verify_outbox(active, invalid_cursor); });
    auto false_ack = populated_state();
    false_ack["outbox"]["destination"] = queue.at("destination");
    false_ack["outbox"]["delivery"] = delivery;
    rejects("invalid-outbox", [&] { verify_outbox(active, false_ack); });
    const auto reopened = parse_json(value.dump());
    check(reopened == value, "acknowledged state changed through serialization");
    verify(active, reopened);
}

} // namespace

int main() {
    try {
        disabled_outbox_stays_null();
        appends_and_coalesces_without_mutating_entries();
        capacity_rejection_preserves_fifo_and_requires_resync();
        byte_capacity_rejects_before_entry_capacity();
        verifier_rejects_tampering();
        serialization_preserves_restart_state();
        reserved_state_budget_and_rejection_ranges();
        metrics_track_retained_samples();
        committed_receipts_drain_without_reusing_sequence();
        std::cout << "observer outbox tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
