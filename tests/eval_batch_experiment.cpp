#include "eval_batch_experiment.h"

#include "row_batch_experiment.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace shin::context::experiment {
namespace {

constexpr std::size_t kMaxRequests = 8;
constexpr std::size_t kMaxBatchRows = 20000;
constexpr std::size_t kMaxBatchPairs = 65536;
constexpr std::size_t kWindowRows = 256;
constexpr std::size_t kMaxRowPayload = 32U * 1024U * 1024U;

struct Metrics {
    std::size_t stages = 0;
    std::size_t serial_fallbacks = 0;
    std::size_t windows = 0;
    std::size_t rowloads = 0;
    std::size_t score_pairs = 0;
    std::size_t prepared_rows = 0;
    std::size_t peak_row_payload_bytes = 0;
    std::size_t pending_count = 0;
    std::size_t error_replays = 0;
    std::vector<Json> stage_trace;
    std::map<std::string, std::size_t> reason_counts;
    std::size_t stage_trace_truncated = 0;
};

constexpr std::size_t kMaxStageTrace = 256;
constexpr std::size_t kMaxTraceQueryBytes = 512;

Metrics& metrics_state() {
    static thread_local Metrics value;
    return value;
}

bool& enabled_state() {
    static thread_local bool value = false;
    return value;
}

std::size_t& batch_size_state() {
    static thread_local std::size_t value = 4;
    return value;
}

enum class Store : unsigned char { section, fact, decision, contract, runbook };
using RowKey = std::pair<Store, int>;

struct OwnedRow {
    bool is_fact = false;
    SectionRow section;
    FactRow fact;
};

std::size_t string_payload(const std::string& value) { return value.capacity() + 1; }
std::size_t strings_payload(const std::vector<std::string>& values) {
    std::size_t size = values.capacity() * sizeof(std::string);
    for (const auto& value : values) size += string_payload(value);
    return size;
}
std::size_t row_payload(const SectionRow& row) {
    return sizeof(row) + string_payload(row.document_id) + string_payload(row.path) + string_payload(row.status) +
        string_payload(row.authority_level) + string_payload(row.heading) + string_payload(row.anchor) +
        string_payload(row.preview) + string_payload(row.indexed_text) + strings_payload(row.topics) +
        string_payload(row.row_type) + string_payload(row.evidence_kind) + string_payload(row.verification_level);
}
std::size_t row_payload(const FactRow& row) {
    return sizeof(row) + string_payload(row.id) + string_payload(row.record_type) + string_payload(row.path) +
        string_payload(row.status) + string_payload(row.area) + string_payload(row.title) +
        string_payload(row.summary) + string_payload(row.source) + string_payload(row.authority_level) +
        strings_payload(row.topics) + strings_payload(row.identifier_terms) + string_payload(row.evidence_kind) +
        string_payload(row.verification_level) + string_payload(row.symbol_name) + string_payload(row.span_hash) +
        string_payload(row.range_origin);
}

std::size_t row_count(const CandidateRows& candidates) {
    return candidates.sections.size() + candidates.facts.size() + candidates.decisions.size() +
        candidates.contracts.size() + candidates.runbooks.size();
}

CandidateRows union_candidates(const std::vector<PendingStage*>& stages) {
    CandidateRows united;
    united.from_terms_index = true;
    united.term_index_coverage_complete = true;
    for (const auto* stage : stages) {
        const auto& c = stage->candidates();
        united.sections.insert(c.sections.begin(), c.sections.end());
        united.facts.insert(c.facts.begin(), c.facts.end());
        united.decisions.insert(c.decisions.begin(), c.decisions.end());
        united.contracts.insert(c.contracts.begin(), c.contracts.end());
        united.runbooks.insert(c.runbooks.begin(), c.runbooks.end());
    }
    united.discovered_row_count = row_count(united);
    return united;
}

std::string stage_ineligible_reason(const PendingStage& stage) {
    const auto& c = stage.candidates();
    const std::size_t count = row_count(c);
    if (!c.from_terms_index) return "not-indexed";
    if (!c.term_index_coverage_complete) return "incomplete-index-coverage";
    if (count != c.discovered_row_count) return "candidate-count-mismatch";
    if (count > kMaxBatchRows) return "query-row-limit";
    return {};
}

std::string batch_ineligible_reason(const std::vector<PendingStage*>& stages,
                                    const CandidateRows& united,
                                    std::size_t& pair_count) {
    pair_count = 0;
    for (const auto* stage : stages) {
        const std::string reason = stage_ineligible_reason(*stage);
        if (!reason.empty()) return reason;
        const std::size_t count = row_count(stage->candidates());
        if (pair_count > kMaxBatchPairs - std::min(count, kMaxBatchPairs)) return "pair-limit";
        pair_count += count;
        if (pair_count > kMaxBatchPairs) return "pair-limit";
    }
    if (!united.from_terms_index) return "union-not-indexed";
    if (!united.term_index_coverage_complete) return "union-incomplete-index-coverage";
    if (united.discovered_row_count > kMaxBatchRows) return "group-union-limit";
    return {};
}

void record_stage_trace(const PendingStage& stage, const std::string& dispatch_reason) {
    Metrics& metrics = metrics_state();
    ++metrics.reason_counts[dispatch_reason];
    if (metrics.stage_trace.size() >= kMaxStageTrace) {
        ++metrics.stage_trace_truncated;
        return;
    }
    const std::string& query = stage.request().query;
    const bool truncated = query.size() > kMaxTraceQueryBytes;
    metrics.stage_trace.push_back({
        {"query", truncated ? query.substr(0, kMaxTraceQueryBytes) : query},
        {"query_truncated", truncated},
        {"required_group_matches", stage.required_group_matches()},
        {"fallback", stage.fallback_used()},
        {"from_terms_index", stage.candidates().from_terms_index},
        {"coverage_complete", stage.candidates().term_index_coverage_complete},
        {"discovered_rows", stage.candidates().discovered_row_count},
        {"sum_ids", row_count(stage.candidates())},
        {"dispatch_reason", dispatch_reason}});
}

void process_batch(const RuntimeOptions& options,
                   const std::vector<PendingStage*>& stages,
                   const CandidateRows& united,
                   std::size_t pair_count) {
    const std::size_t score_pairs_before = metrics_state().score_pairs;
    std::map<RowKey, std::vector<PendingStage*>> membership;
    for (PendingStage* stage : stages) {
        const auto& c = stage->candidates();
        auto add = [&](Store store, const std::set<int>& ids) {
            for (int id : ids) membership[{store, id}].push_back(stage);
        };
        add(Store::section, c.sections);
        add(Store::fact, c.facts);
        add(Store::decision, c.decisions);
        add(Store::contract, c.contracts);
        add(Store::runbook, c.runbooks);
    }

    std::vector<RowKey> ordered_keys;
    ordered_keys.reserve(united.discovered_row_count);
    auto append_keys = [&](Store store, const std::set<int>& ids) {
        for (int id : ids) {
            if (id < 0) throw std::runtime_error("indexed candidate contains a negative row identity");
            ordered_keys.emplace_back(store, id);
        }
    };
    append_keys(Store::section, united.sections);
    append_keys(Store::fact, united.facts);
    append_keys(Store::decision, united.decisions);
    append_keys(Store::contract, united.contracts);
    append_keys(Store::runbook, united.runbooks);

    for (std::size_t first = 0; first < ordered_keys.size(); first += kWindowRows) {
        const std::size_t last = std::min(first + kWindowRows, ordered_keys.size());
        std::map<RowKey, OwnedRow> pool;
        std::size_t payload_bytes = 0;
        for (Store store : {Store::section, Store::fact, Store::decision, Store::contract, Store::runbook}) {
            CandidateRows one;
            one.from_terms_index = true;
            one.term_index_coverage_complete = true;
            std::set<int>* ids = nullptr;
            switch (store) {
                case Store::section: ids = &one.sections; break;
                case Store::fact: ids = &one.facts; break;
                case Store::decision: ids = &one.decisions; break;
                case Store::contract: ids = &one.contracts; break;
                case Store::runbook: ids = &one.runbooks; break;
            }
            for (std::size_t index = first; index < last; ++index)
                if (ordered_keys[index].first == store) ids->insert(ordered_keys[index].second);
            if (ids->empty()) continue;
            one.discovered_row_count = ids->size();
            std::size_t callbacks = 0;
            auto expected = ids->begin();
            auto add_row = [&](const auto& decoded) {
                if (expected == ids->end()) throw std::runtime_error("typed row callback identity/order mismatch");
                const RowKey key{store, *expected};
                ++expected;
                ++callbacks;
                OwnedRow owned;
                std::size_t owned_bytes = 0;
                if constexpr (std::is_same_v<std::decay_t<decltype(decoded)>, SectionRow>) {
                    owned.section = decoded;
                    owned_bytes = row_payload(owned.section) + sizeof(OwnedRow) - sizeof(SectionRow);
                } else {
                    owned.is_fact = true;
                    owned.fact = decoded;
                    owned_bytes = row_payload(owned.fact) + sizeof(OwnedRow) - sizeof(FactRow);
                }
                if (owned_bytes > kMaxRowPayload - payload_bytes)
                    throw std::runtime_error("row window exceeds 32 MiB estimated owned payload");
                payload_bytes += owned_bytes;
                if (!pool.emplace(key, std::move(owned)).second)
                    throw std::runtime_error("duplicate typed row identity in batch window");
            };
            if (store == Store::section) {
                for_each_generated_section_matching_terms(options, one, {}, 0,
                    [&](const SectionRow& row) { add_row(row); });
            } else {
                for_each_generated_fact_matching_terms(options, one, {}, 0,
                    [&](const FactRow& row) { add_row(row); });
            }
            if (callbacks != ids->size() || expected != ids->end())
                throw std::runtime_error("typed row callback count mismatch");
        }

        for (auto& [key, row] : pool) {
            const auto found = membership.find(key);
            if (found == membership.end()) continue;
            Metrics& m = metrics_state();
            m.score_pairs += found->second.size();
            ++m.prepared_rows;
            if (row.is_fact) {
                PreparedRowScope scope(row.fact);
                for (PendingStage* stage : found->second) {
                    MatchScore match = score_fact_match(row.fact, stage->request().prepared_term_groups,
                                                        stage->required_group_matches());
                    // Each stage result is accumulated in its awaiter-owned scratch result.
                    BriefResult& result = stage->scoring_result();
                    push_brief_fact_match(result, row.fact, match, stage->request().limit,
                                          stage->request().per_doc_limit);
                }
            } else {
                PreparedRowScope scope(row.section);
                for (PendingStage* stage : found->second) {
                    MatchScore match = score_section_match(row.section, stage->request().prepared_term_groups,
                                                           stage->required_group_matches());
                    BriefResult& result = stage->scoring_result();
                    push_brief_section_match(result, row.section, match, stage->request().per_doc_limit);
                }
            }
        }
        Metrics& m = metrics_state();
        ++m.windows;
        m.rowloads += pool.size();
        m.peak_row_payload_bytes = std::max(m.peak_row_payload_bytes, payload_bytes);
    }
    if (metrics_state().score_pairs - score_pairs_before != pair_count)
        throw std::logic_error("batch scorer did not score every candidate pair exactly once");
}

} // namespace

BriefTask BriefTask::promise_type::get_return_object() noexcept {
    return BriefTask{Handle::from_promise(*this)};
}

BriefTask& BriefTask::operator=(BriefTask&& other) noexcept {
    if (this != &other) {
        if (handle_) handle_.destroy();
        handle_ = std::exchange(other.handle_, {});
    }
    return *this;
}

BriefTask::~BriefTask() noexcept {
    if (handle_) handle_.destroy();
}

BriefResult BriefTask::take_result() {
    if (!handle_) throw std::logic_error("empty brief task");
    auto& promise = handle_.promise();
    if (promise.error) std::rethrow_exception(promise.error);
    if (!promise.result) throw std::logic_error("brief task completed without a result");
    return std::move(*promise.result);
}

StageAwaiter::StageAwaiter(const RuntimeOptions& options,
                           const QueryRequest& request,
                           CandidateRows candidates,
                           int required_group_matches,
                           bool fallback_used)
    : options_(options), request_(request), candidates_(std::move(candidates)),
      required_group_matches_(required_group_matches), fallback_used_(fallback_used) {
    scoring_result_.fallback_used = fallback_used_;
    scoring_result_.required_group_matches = required_group_matches_;
    scoring_result_.total_term_groups = strict_required_group_matches(request_);
    scoring_result_.candidate_rows = candidates_.discovered_row_count;
    scoring_result_.candidate_index_used = candidates_.from_terms_index;
}

BriefResult StageAwaiter::await_resume() {
    if (!result_) throw std::logic_error("brief stage resumed without a result");
    return std::move(*result_);
}

void configure_eval_batch(bool enabled, std::size_t batch_size) {
    if (batch_size < 1 || batch_size > kMaxRequests)
        throw std::invalid_argument("eval batch size must be between one and eight");
    enabled_state() = enabled;
    batch_size_state() = batch_size;
    metrics_state() = {};
}

bool eval_batch_enabled() noexcept { return enabled_state(); }
std::size_t eval_batch_size() noexcept { return batch_size_state(); }

Json batch_metrics() {
    const Metrics& m = metrics_state();
    Json reason_counts = Json::object();
    for (const auto& [reason, count] : m.reason_counts) reason_counts[reason] = count;
    return Json{{"stages", m.stages}, {"serial_fallbacks", m.serial_fallbacks},
                {"windows", m.windows}, {"rowloads", m.rowloads},
                {"score_pairs", m.score_pairs}, {"prepared_rows", m.prepared_rows},
                {"peak_row_payload_bytes", m.peak_row_payload_bytes},
                {"pending_count", m.pending_count}, {"stage_trace", m.stage_trace},
                {"reason_counts", std::move(reason_counts)},
                {"stage_trace_truncated", m.stage_trace_truncated},
                {"error_replays", m.error_replays}};
}

std::vector<BriefResult> run_brief_tasks(const RuntimeOptions& options,
                                         const std::vector<QueryRequest>& requests,
                                         const Json* manifest,
                                         bool batch) {
    if (requests.size() > kMaxRequests) throw std::invalid_argument("eval batch accepts at most eight requests");
    struct PendingMetricReset {
        ~PendingMetricReset() { metrics_state().pending_count = 0; }
    } pending_metric_reset;
    if (!batch) {
        std::vector<BriefResult> results;
        results.reserve(requests.size());
        for (const auto& request : requests) {
            BriefTask task = collect_brief_task(options, request, manifest);
            if (!task.done()) task.handle().resume();
            while (!task.done()) {
                auto& promise = task.handle().promise();
                PendingStage* stage = promise.pending;
                if (!stage) throw std::logic_error("brief task suspended without a pending stage");
                metrics_state().pending_count = 1;
                ++metrics_state().stages;
                ++metrics_state().serial_fallbacks;
                record_stage_trace(*stage, "control");
                BriefResult result = direct_brief_stage(stage->options(), stage->request(),
                    stage->candidates(), stage->required_group_matches(), stage->fallback_used());
                stage->set_result(std::move(result));
                promise.pending = nullptr;
                metrics_state().pending_count = 0;
                task.handle().resume();
            }
            results.push_back(task.take_result());
        }
        return results;
    }
    std::vector<BriefTask> tasks;
    try {
        tasks.reserve(requests.size());
        for (const auto& request : requests) tasks.push_back(collect_brief_task(options, request, manifest));
        auto throw_first_task_error = [&] {
            for (const auto& task : tasks) {
                if (task.handle() && task.handle().promise().error)
                    std::rethrow_exception(task.handle().promise().error);
            }
        };
        for (auto& task : tasks) if (!task.done()) task.handle().resume();
        throw_first_task_error();

        for (;;) {
            std::vector<std::pair<std::size_t, PendingStage*>> pending;
            for (std::size_t i = 0; i < tasks.size(); ++i) {
                if (tasks[i].done()) continue;
                auto& promise = tasks[i].handle().promise();
                if (!promise.pending) throw std::logic_error("brief task suspended without a pending stage");
                pending.emplace_back(i, promise.pending);
            }
            if (pending.empty()) break;
            metrics_state().pending_count = pending.size();

            for (std::size_t first = 0; first < pending.size();) {
                std::size_t last = first;
                const bool fallback = pending[first].second->fallback_used();
                const std::size_t cap = batch_size_state();
                const bool individually_eligible = stage_ineligible_reason(*pending[first].second).empty();
                std::vector<PendingStage*> group;
                while (last < pending.size() && last < first + cap &&
                       pending[last].second->fallback_used() == fallback &&
                       stage_ineligible_reason(*pending[last].second).empty() == individually_eligible) {
                    group.push_back(pending[last].second);
                    ++last;
                }
                Metrics& m = metrics_state();
                m.stages += group.size();
                CandidateRows united = union_candidates(group);
                std::size_t pairs = 0;
                const std::string ineligible_reason = batch_ineligible_reason(group, united, pairs);
                const std::string dispatch_reason = ineligible_reason.empty() ? "indexed-batch" : ineligible_reason;
                for (PendingStage* stage : group)
                    record_stage_trace(*stage, individually_eligible ? dispatch_reason : stage_ineligible_reason(*stage));
                if (ineligible_reason.empty()) {
                    process_batch(options, group, united, pairs);
                    for (PendingStage* stage : group) {
                        stage->set_result(stage->take_scoring_result());
                    }
                } else {
                    ++m.serial_fallbacks;
                    for (PendingStage* stage : group) {
                        stage->set_result(direct_brief_stage(stage->options(), stage->request(),
                            stage->candidates(), stage->required_group_matches(), stage->fallback_used()));
                    }
                }
                for (std::size_t slot = first; slot < last; ++slot) {
                    auto& promise = tasks[pending[slot].first].handle().promise();
                    promise.pending = nullptr;
                    tasks[pending[slot].first].handle().resume();
                    throw_first_task_error();
                }
                first = last;
            }
        }
    } catch (...) {
        const std::exception_ptr batch_error = std::current_exception();
        ++metrics_state().error_replays;
        tasks.clear(); // Destroy every suspended frame before the ordered serial retry.
        metrics_state().pending_count = 0;
        try {
            (void)run_brief_tasks(options, requests, manifest, false);
        } catch (...) {
            throw; // The first request-order serial error takes precedence.
        }
        std::rethrow_exception(batch_error);
    }

    std::vector<BriefResult> results;
    results.reserve(tasks.size());
    for (auto& task : tasks) results.push_back(task.take_result());
    return results;
}

} // namespace shin::context::experiment
