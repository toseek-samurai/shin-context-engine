#include "shin/server/journal_writer.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace shin::server {
namespace {
void add(std::uint64_t& target, std::uint64_t value = 1) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}
std::uint64_t number(const Json& value) {
    if (!value.is_number_integer() || value < 0 || value > INT64_MAX)
        throw ServiceError(500, "journal-writer-invalid-data", "Journal writer data exceeds its contract.");
    return value.get<std::uint64_t>();
}
std::string reason(std::string_view code) {
    if (code.empty() || code.size() > 64 || !std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    })) return "journal-writer-unavailable";
    return std::string(code);
}
bool terminal(const ServiceError& error) {
    return error.status < 500 || error.code == "journal-invalid-batch" || error.code == "journal-writer-invalid-data" ||
        error.code == "journal-writer-invalid-batch" || error.code == "journal-writer-invalid-receipt" ||
        error.code == "server-identity-mismatch" || error.code == "database-configuration-invalid" ||
        error.code == "schema-unsupported" || error.code == "journal-epoch-exhausted" ||
        error.code == "database-corrupt" || error.code == "journal-progress-inconsistent";
}
} // namespace

JournalWriter::JournalWriter(JournalWriterConfig policy, Snapshotter snapshot, Exporter export_batch)
    : policy_(std::move(policy)), snapshot_(std::move(snapshot)), export_(std::move(export_batch)) {
    if (policy_.interval_ms < 100 || policy_.interval_ms > 60000 ||
        policy_.max_retry_ms < policy_.interval_ms || policy_.max_retry_ms > 300000 || !snapshot_ || !export_)
        throw ServiceError(400, "invalid-journal-writer-policy", "Journal writer policy is invalid.");
}

JournalWriter::~JournalWriter() { shutdown(); }

void JournalWriter::start() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    std::lock_guard lock(mutex_);
    if (started_ || stopped_)
        throw ServiceError(409, "journal-writer-already-started", "Journal writer lifetime cannot restart.");
    started_ = true;
    if (policy_.enabled) worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
}

void JournalWriter::shutdown() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    stopped_ = true;
}

Json JournalWriter::status() const {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    const auto remaining = std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(next_attempt_ - now).count());
    const auto state = !policy_.enabled ? "disabled" : stopped_ ? "stopped" : !started_ ? "not-started" :
        blocked_ ? "blocked" : in_flight_ ? "exporting" : consecutive_failures_ != 0 ? "retry-wait" : "waiting";
    return {{"schema_version", "shin.journal-writer-status.v1"}, {"enabled", policy_.enabled}, {"state", state},
        {"scope", "process-lifetime"}, {"read_only", true}, {"automatic_retention", false},
        {"pending_batches", pending_bytes_ == 0 ? 0 : 1}, {"pending_records", pending_records_},
        {"pending_bytes", pending_bytes_}, {"pending_through", pending_through_},
        {"maximum_batch_records", maximum_records}, {"maximum_batch_bytes", maximum_batch_bytes},
        {"sampled_through", sampled_through_}, {"acknowledged_through", acknowledged_through_},
        {"unacknowledged_sequences_at_last_poll", sampled_through_ > acknowledged_through_ ? sampled_through_ - acknowledged_through_ : 0},
        {"source_progress_is_live", false}, {"acknowledgement_includes_missing", true},
        {"source_sample_age_ms", sampled_at_ == Clock::time_point{} ? Json(nullptr) :
            Json(std::chrono::duration_cast<std::chrono::milliseconds>(now - sampled_at_).count())},
        {"acknowledged_producer_epoch", producer_epoch_ == 0 ? Json(nullptr) : Json(producer_epoch_)},
        {"snapshot_polls", polls_}, {"export_attempts", attempts_}, {"export_successes", successes_},
        {"failures", failures_}, {"consecutive_failures", consecutive_failures_},
        {"inserted_records", inserted_}, {"new_missing_sequences", new_missing_}, {"recovered_missing_sequences", recovered_missing_},
        {"next_attempt_in_ms", !policy_.enabled || !started_ || stopped_ || blocked_ || in_flight_ ? Json(nullptr) : Json(remaining)},
        {"last_failure", failure_code_.empty() ? Json(nullptr) : Json{{"code", failure_code_}, {"stage", failure_stage_}}},
        {"shutdown_prefix_acknowledged", shutdown_prefix_acknowledged_}, {"all_traffic_complete", false}};
}

bool JournalWriter::load_pending() {
    if (!pending_.is_null()) return true;
    {
        std::lock_guard lock(mutex_);
        add(polls_);
    }
    auto batch = snapshot_(acknowledged_through_);
    const auto through = number(batch.at("latest_sequence"));
    const auto& instance = batch.at("instance_id");
    const auto& records = batch.at("records");
    if (!instance.is_string() || instance.get_ref<const std::string&>().size() != 64 ||
        instance.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") != std::string::npos ||
        !records.is_array() || records.size() > maximum_records)
        throw ServiceError(500, "journal-writer-invalid-batch", "Journal writer batch is invalid.");
    const auto bytes = batch.dump().size();
    if (bytes > maximum_batch_bytes)
        throw ServiceError(500, "journal-writer-invalid-batch", "Journal writer batch exceeds its bound.");
    std::lock_guard lock(mutex_);
    if ((!instance_.empty() && instance_ != instance.get_ref<const std::string&>()) || through < sampled_through_)
        throw ServiceError(500, "journal-writer-invalid-batch", "Journal source lifetime changed.");
    instance_ = instance.get<std::string>();
    sampled_through_ = through;
    sampled_at_ = Clock::now();
    if (through <= acknowledged_through_) return false;
    pending_records_ = records.size();
    pending_bytes_ = bytes;
    pending_through_ = through;
    pending_ = std::move(batch);
    return true;
}

void JournalWriter::failed(const ServiceError& error, std::string_view stage) {
    std::lock_guard lock(mutex_);
    add(failures_);
    add(consecutive_failures_);
    failure_code_ = reason(error.code);
    failure_stage_ = stage;
    blocked_ = terminal(error);
    in_flight_ = false;
}

JournalWriter::Step JournalWriter::step() {
    std::string_view stage = "snapshot";
    try {
        if (!load_pending()) {
            std::lock_guard lock(mutex_);
            consecutive_failures_ = 0;
            failure_code_.clear();
            failure_stage_.clear();
            return Step::idle;
        }
        stage = "export";
        {
            std::lock_guard lock(mutex_);
            in_flight_ = true;
            add(attempts_);
        }
        const auto receipt = export_(pending_);
        stage = "receipt";
        const auto through = number(receipt.at("observed_through"));
        const auto epoch = number(receipt.at("producer_epoch"));
        const auto inserted = number(receipt.at("inserted_count"));
        const auto missing = number(receipt.at("new_missing_sequences"));
        const auto recovered = number(receipt.at("recovered_missing_sequences"));
        if (receipt.at("schema_version") != "shin.retrieval-journal-capture.v1" ||
            receipt.at("instance_id") != pending_.at("instance_id") ||
            through < number(pending_.at("latest_sequence")) || epoch == 0 ||
            inserted > pending_.at("records").size())
            throw ServiceError(500, "journal-writer-invalid-receipt", "Journal capture acknowledgement is invalid.");
        std::lock_guard lock(mutex_);
        if (producer_epoch_ != 0 && producer_epoch_ != static_cast<std::int64_t>(epoch))
            throw ServiceError(500, "journal-writer-invalid-receipt", "Journal capture epoch changed.");
        producer_epoch_ = static_cast<std::int64_t>(epoch);
        acknowledged_through_ = std::max(through, acknowledged_through_);
        add(successes_);
        add(inserted_, inserted);
        add(new_missing_, missing);
        add(recovered_missing_, recovered);
        consecutive_failures_ = 0;
        failure_code_.clear();
        failure_stage_.clear();
        in_flight_ = false;
        pending_ = nullptr;
        pending_records_ = 0;
        pending_bytes_ = 0;
        pending_through_ = 0;
        return Step::exported;
    } catch (const ServiceError& error) {
        failed(error, stage);
    } catch (const Json::exception&) {
        failed(ServiceError(500, stage == "snapshot" ? "journal-writer-invalid-batch" :
                            stage == "receipt" ? "journal-writer-invalid-receipt" : "journal-writer-invalid-data",
                            "Journal writer data is invalid."), stage);
    } catch (...) {
        failed(ServiceError(503, "journal-writer-unavailable", "Journal writer is unavailable."), stage);
    }
    std::lock_guard lock(mutex_);
    return blocked_ ? Step::blocked : Step::failed;
}

void JournalWriter::work(std::stop_token stop) {
    bool failed_during_shutdown = false;
    while (!stop.stop_requested()) {
        const auto outcome = step();
        if (stop.stop_requested() && outcome == Step::failed) {
            failed_during_shutdown = true;
            break;
        }
        std::unique_lock lock(mutex_);
        if (outcome == Step::blocked) {
            wake_.wait(lock, stop, [] { return false; });
            break;
        }
        auto delay = policy_.interval_ms;
        for (std::uint64_t i = 0; i < std::min<std::uint64_t>(consecutive_failures_, 12); ++i)
            delay = std::min(policy_.max_retry_ms, delay * 2);
        next_attempt_ = Clock::now() + std::chrono::milliseconds(delay);
        wake_.wait_until(lock, stop, next_attempt_, [] { return false; });
    }
    // An in-flight failure after stop is already a failed shutdown attempt.
    // Otherwise HTTP has drained: retry pending data and one final tail at most.
    if (policy_.flush_on_shutdown && !blocked_ && !failed_during_shutdown) {
        for (int i = 0; i < 2; ++i) if (step() != Step::exported) break;
    }
    try {
        if (!blocked_) static_cast<void>(load_pending());
    } catch (...) {
        failed(ServiceError(503, "journal-writer-shutdown-unobserved", "Journal shutdown source is unavailable."), "snapshot");
        return;
    }
    std::lock_guard lock(mutex_);
    shutdown_prefix_acknowledged_ = !blocked_ && pending_.is_null() && acknowledged_through_ >= sampled_through_;
}

} // namespace shin::server
