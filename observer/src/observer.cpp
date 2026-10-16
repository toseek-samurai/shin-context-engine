#include "shin/observer/observer.h"
#include "shin/observer/outbox.h"
#include "shin/observer/metrics.h"
#include "shin/observer/delivery.h"
#include "shin/observer_protocol.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#endif
#include <array>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <thread>

namespace shin::observer {
namespace {
constexpr std::uint64_t kMaximumSequence = std::numeric_limits<std::int64_t>::max() - 1;

std::string new_epoch() {
    std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
#else
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
#endif
        throw Error("epoch-failed", "Cannot create an observation epoch.");
    std::string result;
    for (const auto byte : bytes) {
        result.push_back("0123456789abcdef"[byte >> 4]);
        result.push_back("0123456789abcdef"[byte & 15]);
    }
    return result;
}

void verify_state(const Config& config, const Json& state) {
    const std::set<std::string> keys{"schema_version", "binding_key", "binding", "root_identity", "sequence", "epoch",
        "lifecycle", "previous_run_interrupted", "observation", "producer_version", "core_version", "outbox", "metrics"};
    if (!state.is_object() || state.size() != keys.size()) throw Error("invalid-state", "Observer state is invalid.");
    for (const auto& key : keys) if (!state.contains(key)) throw Error("invalid-state", "Observer state is invalid.");
    if (state.at("schema_version") != "shin.observer-state.v3") throw Error("invalid-state", "Unsupported Observer state; preserve old state and choose a new state directory.");
    if (state.at("binding_key") != config.binding_key || state.at("binding") != config.binding)
        throw Error("state-binding-mismatch", "State belongs to another Observer binding.");
    const auto& sequence = state.at("sequence");
    const auto& epoch = state.at("epoch");
    const auto& identity = state.at("root_identity");
    if (!sequence.is_number_integer() || sequence < 0 || sequence > kMaximumSequence ||
        !epoch.is_string() || epoch.get_ref<const std::string&>().size() != 32 ||
        epoch.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") != std::string::npos ||
        !identity.is_string() || identity.get_ref<const std::string&>().empty() || identity.get_ref<const std::string&>().size() > 128 ||
        !state.at("previous_run_interrupted").is_boolean() || !state.at("producer_version").is_string() ||
        !state.at("core_version").is_string() ||
        (state.at("lifecycle") != "assessing" && state.at("lifecycle") != "waiting" && state.at("lifecycle") != "stopped"))
        throw Error("invalid-state", "Observer state values are invalid.");
    for (const auto* key : {"producer_version", "core_version"}) {
        const auto& version = state.at(key).get_ref<const std::string&>();
        if (version.empty() || version.size() > 64 || version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz.-") != std::string::npos)
            throw Error("invalid-state", "Stored producer version is invalid.");
    }
    const auto& root = identity.get_ref<const std::string&>();
    constexpr std::string_view windows_prefix = "win-volume-v1:";
    if (root.starts_with(windows_prefix)) {
        const auto value = root.substr(windows_prefix.size());
        bool valid = value.size() >= 71 && value[36] == ':' && value[69] == ':';
        for (std::size_t i = 0; valid && i < 36; ++i) {
            const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
            valid = separator ? value[i] == '-' : std::string_view("0123456789abcdef").find(value[i]) != std::string_view::npos;
        }
        valid = valid && value.substr(37, 32).find_first_not_of("0123456789abcdef") == std::string::npos &&
            value.substr(70).find_first_not_of("0123456789") == std::string::npos && value[70] != '0';
        if (!valid) throw Error("invalid-state", "Stored Windows directory identity is invalid.");
    } else {
        constexpr std::string_view volume_prefix = "mac-volume-v1:";
        const bool volume_id = root.starts_with(volume_prefix);
        const std::size_t offset = volume_id ? volume_prefix.size() + 32 + 1 : 0;
        bool valid_identity = true;
        if (volume_id) {
            valid_identity = root.size() > offset && root[offset - 1] == ':' &&
                root.substr(volume_prefix.size(), 32).find_first_not_of("0123456789abcdef") == std::string::npos;
        }
        const auto numeric = root.substr(std::min(offset, root.size()));
        const auto separators = std::count(numeric.begin(), numeric.end(), ':');
        if (!valid_identity || numeric.empty() || numeric.front() == ':' || numeric.back() == ':' ||
            numeric.find("::") != std::string::npos || separators != (volume_id ? 2 : 1) ||
            numeric.find_first_not_of("0123456789:") != std::string::npos)
            throw Error("invalid-state", "Stored source directory identity is invalid.");
    }
    verify_observation(state.at("observation"));
    if ((sequence == 0) != state.at("observation").is_null() ||
        (state.at("lifecycle") == "waiting" && sequence == 0))
        throw Error("invalid-state", "Observer sequence and observation are inconsistent.");
    verify_outbox(config, state);
    verify_metrics(state);
}

void pause(const Config& config, const std::function<bool()>& stopped, const std::function<void()>& tick) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config.poll_interval_ms);
    while (!stopped() && std::chrono::steady_clock::now() < deadline) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
} // namespace

void verify_observation(const Json& observation) {
    try {
        observer_protocol::verify_observation(observation);
    } catch (const observer_protocol::ValidationError& error) {
        throw Error("invalid-state", error.what());
    }
}

Json response(const Json& result) {
    return {{"schema_version", "shin.observer-response.v1"}, {"tool", "shin-observer"},
        {"version", SHIN_OBSERVER_VERSION}, {"core_version", SHIN_VERSION}, {"stability", SHIN_OBSERVER_STABILITY},
        {"ok", true}, {"result", result}};
}

Json run_observer(const Config& config, std::uint64_t samples, const std::function<bool()>& stopped, const DeliveryConfig* delivery) {
    StateStore store(config.state_dir, true, config.binding.at("state_identity").get<std::string>());
    auto loaded = store.load();
    if (loaded) verify_state(config, *loaded);
    else {
        std::error_code error;
        const auto pending = fs::symlink_status(config.state_dir / "state.pending", error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw Error("uncommitted-state", "Initial Observer state requires review.");
        if (fs::exists(pending)) {
            const auto initial = parse_json(read_text(config.state_dir / "state.pending", 256 * 1024, true));
            verify_state(config, initial);
            if (initial.at("sequence") != 0 || !initial.at("observation").is_null())
                throw Error("uncommitted-state", "An uncommitted observation cannot replace missing committed state.");
        }
    }
    store.recover_pending();
    Json state = loaded ? *loaded : Json{{"schema_version", "shin.observer-state.v3"},
        {"binding_key", config.binding_key}, {"binding", config.binding},
        {"root_identity", directory_identity(config.root)}, {"sequence", 0}, {"observation", nullptr},
        {"outbox", new_outbox(config, new_epoch())}, {"metrics", new_metrics()}};
    state["previous_run_interrupted"] = loaded && loaded->at("lifecycle") != "stopped";
    state["epoch"] = new_epoch();
    state["producer_version"] = SHIN_OBSERVER_VERSION;
    state["core_version"] = SHIN_VERSION;
    state["lifecycle"] = "assessing";
    if (delivery) bind_delivery(state.at("outbox"), *delivery);
    verify_state(config, state);
    store.save(state);
    std::unique_ptr<DeliverySession> sender;
    if (delivery) sender = std::make_unique<DeliverySession>(config, *delivery);
    const auto tick = [&] { if (sender) sender->tick(state, store); };
    tick();
    for (std::uint64_t count = 0; !stopped() && (samples == 0 || count < samples); ++count) {
        if (state.at("sequence").get<std::uint64_t>() >= kMaximumSequence)
            throw Error("sequence-exhausted", "Observer sequence capacity was reached.");
        if (count != 0) {
            state["lifecycle"] = "assessing";
            store.save(state);
        }
        state["observation"] = sample_worktree(config, state.at("root_identity").get<std::string>());
        state["sequence"] = state.at("sequence").get<std::uint64_t>() + 1;
        record_metrics(state.at("metrics"), state.at("observation"));
        capture_outbox(config, state);
        state["lifecycle"] = "waiting";
        verify_state(config, state);
        store.save(state);
        tick();
        if (samples == 0 || count + 1 < samples) pause(config, stopped, tick);
    }
    if (sender) sender->finish(state, store);
    state["lifecycle"] = "stopped";
    store.save(state);
    return state;
}

Json observer_status(const Config& config) {
    StateStore store(config.state_dir, false, config.binding.at("state_identity").get<std::string>());
    const auto state = store.load();
    if (state) verify_state(config, *state);
    Json age = nullptr;
    std::string age_status = "unavailable";
    if (state && !state->at("observation").is_null()) {
        const auto captured = state->at("observation").at("observed_at_ms").get<std::int64_t>();
        const auto current = now_ms();
        if (current < 0 || captured > current) age_status = "time-uncertain";
        else {
            age = current - captured;
            age_status = age <= std::max<std::uint64_t>(1000, config.poll_interval_ms * 3) ? "within-ttl" : "expired";
        }
    }
    return {{"state", state ? *state : Json(nullptr)}, {"writer_lock_held", store.writer_present()},
        {"live_source_verified", false}, {"observation_age_ms", age}, {"observation_age", age_status},
        {"observer_state_mutated", false}, {"consumer_mutated", false}, {"index_mutated", false},
        {"synchronization", "explicit-metadata-delivery"}};
}

Json observer_metrics(const Config& config) {
    const auto status = observer_status(config);
    const auto& state = status.at("state");
    return {{"schema_version", "shin.observer-metrics-result.v1"}, {"state_available", !state.is_null()},
        {"metrics", state.is_null() ? Json(nullptr) : state.at("metrics")},
        {"outbox", state.is_null() ? Json(nullptr) : outbox_summary(config, state.at("outbox"))},
        {"writer_lock_held", status.at("writer_lock_held")}, {"observation_age", status.at("observation_age")},
        {"observation_age_ms", status.at("observation_age_ms")}, {"live_source_verified", false},
        {"state_mutated", false}, {"synchronization", "explicit-metadata-delivery"}};
}

Json archive_outbox(const Config& config, const fs::path& archive, bool verify_only) {
    if (!config.outbox_enabled) throw Error("outbox-disabled", "Archiving requires an enabled outbox.");
    if (!archive.is_absolute() || archive.lexically_normal() != archive || archive == archive.root_path() ||
        archive.string().size() > 4096 || archive.string().find('\0') != std::string::npos)
        throw Error("invalid-archive", "Archive path must be absolute and normalized.");
    for (const auto& excluded : {config.root, config.out_dir, config.profile_dir, config.state_dir}) {
        const auto [left, right] = std::mismatch(archive.begin(), archive.end(), excluded.begin(), excluded.end());
        if (left == archive.end() || right == excluded.end())
            throw Error("invalid-archive", "Archive must be outside source, index, profiles and Observer state.");
    }
    const auto parent_identity = directory_identity(archive.parent_path(), true);
    StateStore store(config.state_dir, true, config.binding.at("state_identity").get<std::string>(), false);
    const auto state = store.load();
    if (!state) throw Error("state-unavailable", "Archiving requires existing committed Observer state.");
    verify_state(config, *state);
    const auto bytes = state->dump();
    const bool created = !verify_only && write_private_archive(archive, bytes);
    const auto saved = read_text(archive, kMaximumStateBytes, true);
    const auto archived = parse_json(saved);
    verify_state(config, archived);
    const auto current = store.load();
    if (saved != bytes || !current || current->dump() != bytes ||
        directory_identity(archive.parent_path(), true) != parent_identity)
        throw Error("archive-stale", "Archive does not match the current committed state; review a fresh export.");
    const auto& queue = state->at("outbox");
    return {{"schema_version", "shin.observer-outbox-archive.v1"}, {"archive_sha256", digest(saved)},
        {"archive_bytes", saved.size()}, {"archive_created", created}, {"binding_key", config.binding_key},
        {"stream_id", queue.at("stream_id")}, {"destination", queue.at("destination")},
        {"observation_sequence", state->at("sequence")}, {"accepted_sequence", queue.at("accepted_sequence")},
        {"acknowledged_sequence", queue.at("acknowledged_sequence")}, {"pending_entries", queue.at("entries").size()},
        {"pending_payload_bytes", queue.at("payload_bytes")}, {"rejected_observations", queue.at("rejected_observations")},
        {"current_state_verified", true}, {"committed_state_only", true}, {"uncommitted_pending_included", false},
        {"observer_state_mutated", false}, {"consumer_mutated", false}, {"index_mutated", false},
        {"delivery_performed", false}, {"external_archive_durability_verified", false}};
}

Json deliver_outbox(const Config& config, const DeliveryConfig& delivery, std::uint64_t maximum,
                    const std::function<bool()>& stopped) {
    if (maximum == 0 || maximum > 128 || !config.outbox_enabled)
        throw Error("invalid-delivery", "Delivery requires an enabled outbox and a bounded batch.");
    StateStore store(config.state_dir, true, config.binding.at("state_identity").get<std::string>());
    auto loaded = store.load();
    if (!loaded) throw Error("state-unavailable", "Sample the local workspace before delivery.");
    verify_state(config, *loaded);
    Json state = *loaded;
    bind_delivery(state.at("outbox"), delivery);
    verify_state(config, state);
    store.recover_pending();
    store.save(state);
    DeliverySession sender(config, delivery);
    for (std::uint64_t count = 0; count < maximum && !stopped() && !state.at("outbox").at("entries").empty(); ++count) {
        sender.tick(state, store);
        sender.finish(state, store);
        if (sender.failed()) break;
    }
    return {{"state", state}, {"attempts_completed", sender.completed()}, {"delivery_failed", sender.failed()},
        {"pending_drained", state.at("outbox").at("entries").empty()}, {"consumer_mutated", false},
        {"index_mutated", false}, {"shared_publication_mutated", false}};
}

Json reconcile_outbox(const Config& config, const DeliveryConfig& delivery) {
    if (!config.outbox_enabled) throw Error("outbox-disabled", "Reconciliation requires an enabled outbox.");
    StateStore store(config.state_dir, false, config.binding.at("state_identity").get<std::string>());
    const auto state = store.load();
    if (!state) throw Error("state-unavailable", "Reconciliation requires existing local state.");
    verify_state(config, *state);
    const auto& queue = state->at("outbox");
    if (queue.at("destination").is_null())
        throw Error("delivery-binding-required", "Reconciliation requires a persisted destination. Explicit delivery binds it before network access.");
    if (queue.at("destination") != delivery.destination)
        throw Error("delivery-binding-mismatch", "Existing delivery destination cannot be changed implicitly.");
    const Json request{{"schema_version", "shin.observer-reconcile-request.v1"}, {"server_id", delivery.destination.at("server_id")},
        {"stream_id", queue.at("stream_id")}, {"acknowledged_sequence", queue.at("acknowledged_sequence")},
        {"accepted_sequence", queue.at("accepted_sequence")}, {"last_receipt", queue.at("last_receipt")},
        {"next_submission", queue.at("entries").empty() ? Json(nullptr) : queue.at("entries").front()}};
    return reconcile_remote(delivery, request);
}
} // namespace shin::observer
