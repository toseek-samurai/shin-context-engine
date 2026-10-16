#include "shin/server/retrieval_telemetry.h"

#include <openssl/rand.h>
#include <algorithm>
#include <limits>
#include <optional>

namespace shin::server {
namespace {
constexpr std::size_t kMaximumPayload = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumRecord = 12U * 1024U;
constexpr std::array<std::uint64_t, 10> kBoundsMs{1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000};
constexpr std::array<const char*, 3> kOperations{"search", "brief", "pack"};
constexpr std::array<const char*, 2> kSelectors{"route", "publication"};
constexpr std::array<const char*, 5> kPhases{"validation", "selection", "admission", "execution", "finalization"};

std::uint64_t elapsed(RetrievalTelemetry::Clock::time_point from, RetrievalTelemetry::Clock::time_point to) {
    return to <= from ? 0 : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

void add(std::uint64_t& target, std::uint64_t value = 1) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}

std::size_t slot(RetrievalOperation operation, RetrievalSelector selector) {
    return static_cast<std::size_t>(selector) * kOperations.size() + static_cast<std::size_t>(operation);
}

std::string new_instance() {
    Digest random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw ServiceError(500, "random-identity-unavailable", "Cannot allocate a telemetry instance identity.");
    return digest_hex(random);
}

std::string reason_code(std::string_view code) {
    if (code.empty() || code.size() > 64 || !std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    })) return "unclassified-error";
    return std::string(code);
}

void number(Json& output, const char* key, const Json& from, const char* field) {
    const auto value = from.find(field);
    if (value != from.end() && value->is_number_integer() && *value >= 0) output[key] = *value;
}

void boolean(Json& output, const char* key, const Json& from, const char* field) {
    const auto value = from.find(field);
    if (value != from.end() && value->is_boolean()) output[key] = *value;
}

Json quality(const Json* result) {
    Json output{{"retrieval_mode", nullptr}, {"returned_records", nullptr}, {"candidate_rows", nullptr},
                {"covered_groups", nullptr}, {"total_groups", nullptr}, {"pack_complete", nullptr},
                {"pack_coverage_complete", nullptr}, {"required_requirements", nullptr},
                {"satisfied_required_requirements", nullptr}};
    if (!result || !result->contains("evidence")) return output;
    const auto& evidence = result->at("evidence");
    if (evidence.contains("retrieval_mode")) {
        const auto& mode = evidence.at("retrieval_mode");
        for (const auto* allowed : {"strict", "relaxed", "owner-scoped", "directory-scoped", "planned", "identifier-scoped"})
            if (mode == allowed) output["retrieval_mode"] = mode;
    }
    if (evidence.contains("evidence") && evidence.at("evidence").is_array())
        output["returned_records"] = evidence.at("evidence").size();
    if (evidence.contains("sections") && evidence.contains("facts") &&
        evidence.at("sections").is_array() && evidence.at("facts").is_array())
        output["returned_records"] = evidence.at("sections").size() + evidence.at("facts").size();
    if (evidence.contains("query_diagnostics"))
        number(output, "candidate_rows", evidence.at("query_diagnostics"), "candidate_rows");
    if (evidence.contains("query_coverage")) {
        const auto& coverage = evidence.at("query_coverage");
        number(output, "covered_groups", coverage, "covered_groups");
        number(output, "total_groups", coverage, "total_groups");
    }
    if (evidence.contains("completion")) {
        const auto& completion = evidence.at("completion");
        boolean(output, "pack_complete", completion, "complete");
        boolean(output, "pack_coverage_complete", completion, "coverage_complete");
        number(output, "required_requirements", completion, "required_requirements");
        number(output, "satisfied_required_requirements", completion, "satisfied_required_requirements");
    }
    return output;
}
} // namespace

void RetrievalTelemetry::Attempt::mark(RetrievalPhase next, Clock::time_point now) noexcept {
    add(phase_ns[static_cast<std::size_t>(phase)], elapsed(last_mark, now));
    last_mark = now;
    phase = next;
    measured[static_cast<std::size_t>(next)] = true;
}

RetrievalTelemetry::RetrievalTelemetry(const ObservabilityConfig& config) : config_(config), instance_(new_instance()) {
    if (config_.max_retained_requests > 256 || config_.max_query_bytes == 0 || config_.max_query_bytes > 8192 ||
        config_.capture_query_projects.size() > 32 || (!config_.capture_query_projects.empty() && config_.max_retained_requests == 0))
        throw ServiceError(500, "invalid-config", "Retrieval observability limits are invalid.");
}

RetrievalTelemetry::Attempt RetrievalTelemetry::begin(RetrievalOperation operation, RetrievalSelector selector,
                                                     Clock::time_point now) noexcept {
    bool tracked = false;
    try {
        std::lock_guard lock(mutex_);
        auto& cell = cells_.at(slot(operation, selector));
        add(cell.started);
        add(cell.active);
        tracked = true;
    } catch (...) { collection_errors_.fetch_add(1, std::memory_order_relaxed); }
    return Attempt(operation, selector, now, tracked);
}

Json RetrievalTelemetry::sample(const Attempt& attempt, std::string_view project, std::string_view name,
                                const Json& request, const Json* result, int status, std::string_view reason,
                                std::uint64_t elapsed_ns) const {
    Json phases = Json::object();
    for (std::size_t i = 0; i < kPhases.size(); ++i)
        phases[kPhases[i]] = attempt.measured[i] ? Json(attempt.phase_ns[i]) : Json(nullptr);
    Json selection = nullptr;
    if (result) {
        selection = Json::object();
        for (const auto* key : {"route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision"})
            selection[key] = result->at(key);
    }
    Json input = nullptr;
    std::string capture = "disabled";
    if (std::find(config_.capture_query_projects.begin(), config_.capture_query_projects.end(), project) !=
        config_.capture_query_projects.end()) {
        capture = "invalid-request";
        if (attempt.request_validated) {
            try {
                if (request.dump().size() <= config_.max_query_bytes) { input = request; capture = "captured"; }
                else capture = "omitted-budget";
            } catch (...) { capture = "capture-failed"; }
        }
    }
    Json record{{"sequence", 0}, {"operation", kOperations[static_cast<std::size_t>(attempt.operation)]},
            {"selector_kind", kSelectors[static_cast<std::size_t>(attempt.selector)]}, {"project_id", project},
            {"selector_id", name}, {"observed_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()},
            {"outcome", status == 200 ? "success" : "error"}, {"status", status},
            {"reason_code", status == 200 ? Json(nullptr) : Json(reason_code(reason))},
            {"last_phase", kPhases[static_cast<std::size_t>(attempt.phase)]},
            {"elapsed_ns", elapsed_ns}, {"result_bytes", attempt.result_bytes ? Json(*attempt.result_bytes) : Json(nullptr)},
            {"phase_ns", std::move(phases)}, {"selection", std::move(selection)},
            {"quality", quality(result)}, {"query_capture", capture}, {"request", std::move(input)}};
    if (attempt.attribution) {
        const auto& a = *attempt.attribution;
        record["attribution"] = {{"principal_id", a.principal_id}, {"tenant_id", a.tenant_id}, {"workspace_id", a.workspace_id}};
    }
    return record;
}

void RetrievalTelemetry::finish(Attempt& attempt, std::string_view project, std::string_view name,
                                const Json& request, const Json* result, int status, std::string_view reason,
                                Clock::time_point now) noexcept {
    if (!attempt.tracked) return;
    attempt.tracked = false;
    attempt.mark(attempt.phase, now);
    const auto duration = elapsed(attempt.started, now);
    std::optional<Record> record;
    if (config_.max_retained_requests != 0) {
        try {
            auto value = sample(attempt, project, name, request, result, status, reason, duration);
            const auto bytes = value.dump().size();
            if (bytes + 19 <= kMaximumRecord) record = Record{std::move(value), bytes};
        } catch (...) { collection_errors_.fetch_add(1, std::memory_order_relaxed); }
    }
    try {
        std::lock_guard lock(mutex_);
        auto& cell = cells_.at(slot(attempt.operation, attempt.selector));
        if (cell.active != 0) --cell.active;
        add(cell.completed);
        add(status == 200 ? cell.succeeded : cell.failed);
        add(cell.total_ns, duration);
        cell.max_ns = std::max(cell.max_ns, duration);
        const auto bucket = std::find_if(kBoundsMs.begin(), kBoundsMs.end(), [&](auto ms) { return duration <= ms * 1000000; });
        add(cell.histogram[static_cast<std::size_t>(bucket - kBoundsMs.begin())]);
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) { add(dropped_); return; }
        ++sequence_;
        if (config_.max_retained_requests == 0) { add(disabled_); return; }
        if (!record) { add(dropped_); return; }
        record->value["sequence"] = sequence_;
        for (auto digits = sequence_; digits >= 10; digits /= 10) ++record->bytes;
        while (!records_.empty() && (records_.size() >= config_.max_retained_requests || record->bytes > kMaximumPayload - bytes_)) {
            bytes_ -= records_.front().bytes;
            records_.pop_front();
            add(evicted_);
        }
        const auto bytes = record->bytes;
        try { records_.push_back(std::move(*record)); bytes_ += bytes; }
        catch (...) { add(dropped_); collection_errors_.fetch_add(1, std::memory_order_relaxed); }
    } catch (...) { collection_errors_.fetch_add(1, std::memory_order_relaxed); }
}

Json RetrievalTelemetry::metrics() const {
    std::lock_guard lock(mutex_);
    Json cells = Json::array();
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        const auto& cell = cells_[i];
        Json histogram = Json::array();
        for (std::size_t b = 0; b < cell.histogram.size(); ++b)
            histogram.push_back({{"upper_bound_ms", b < kBoundsMs.size() ? Json(kBoundsMs[b]) : Json(nullptr)}, {"count", cell.histogram[b]}});
        cells.push_back({{"operation", kOperations[i % 3]}, {"selector_kind", kSelectors[i / 3]},
            {"started", cell.started}, {"completed", cell.completed}, {"active", cell.active},
            {"succeeded", cell.succeeded}, {"failed", cell.failed}, {"elapsed_total_ns", cell.total_ns},
            {"elapsed_max_ns", cell.max_ns}, {"histogram", std::move(histogram)}});
    }
    return {{"schema_version", "shin.retrieval-metrics.v1"}, {"instance_id", instance_},
            {"scope", "process-lifetime-authorized-service-attempts"}, {"counter_policy", "saturating-uint64"},
            {"histogram_encoding", "non-cumulative-upper-inclusive; null upper bound is +Inf"}, {"cells", std::move(cells)},
            {"retention", {{"maximum_records", config_.max_retained_requests}, {"maximum_payload_bytes", kMaximumPayload},
                {"retained_records", records_.size()}, {"retained_payload_bytes", bytes_}, {"evicted", evicted_},
                {"dropped", dropped_}, {"disabled", disabled_}, {"latest_sequence", sequence_}}},
            {"collection_errors", collection_errors_.load(std::memory_order_relaxed)}};
}

Json RetrievalTelemetry::journal_snapshot(std::uint64_t after) const {
    std::lock_guard lock(mutex_);
    Json records = Json::array();
    for (const auto& record : records_) if (record.value.at("sequence") > after) records.push_back(record.value);
    return {{"instance_id", instance_}, {"latest_sequence", sequence_}, {"records", std::move(records)}};
}

Json RetrievalTelemetry::requests(std::uint64_t after, std::size_t limit, std::string_view instance) const {
    if (limit == 0 || limit > 32) throw ServiceError(400, "invalid-query", "Retrieval diagnostic page limit is invalid.");
    if ((after != 0 && instance != instance_) || (!instance.empty() && instance != instance_))
        throw ServiceError(409, "telemetry-instance-mismatch", "Start a new diagnostic cursor for this process instance.");
    std::lock_guard lock(mutex_);
    if (after > sequence_) throw ServiceError(400, "invalid-query", "Retrieval diagnostic cursor exceeds this process history.");
    Json items = Json::array();
    auto cursor = after;
    bool gap = false;
    for (const auto& record : records_) {
        const auto sequence = record.value.at("sequence").get<std::uint64_t>();
        if (sequence > after && items.size() < limit) {
            gap = gap || sequence - cursor > 1;
            cursor = sequence;
            items.push_back(record.value);
        }
    }
    Json next = nullptr;
    if (!items.empty() && items.back().at("sequence") != records_.back().value.at("sequence")) next = items.back().at("sequence");
    if (next.is_null() && cursor < sequence_) gap = true;
    return {{"schema_version", "shin.retrieval-request-page.v1"}, {"instance_id", instance_},
            {"scope", "bounded-process-local-diagnostics"},
            {"oldest_available_sequence", records_.empty() ? Json(nullptr) : records_.front().value.at("sequence")},
            {"latest_sequence", sequence_}, {"gap", gap},
            {"next_after", next}, {"items", std::move(items)}};
}
} // namespace shin::server
