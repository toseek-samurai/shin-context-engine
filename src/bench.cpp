#include "context_internal.h"

namespace shin::context {

namespace {

struct BenchCounts {
    int documents = 0;
    int sections = 0;
    int links = 0;
    int topics = 0;
    int terms = 0;
    int decisions = 0;
    int contracts = 0;
    int runbooks = 0;
    int facts = 0;
    int source_fact_candidate_files = 0;
    int source_fact_scanned_files = 0;
    int source_fact_skipped_large_files = 0;
    int source_fact_capped_files = 0;
    int jsonl_candidate_rows = 0;
    int jsonl_indexed_rows = 0;
    int jsonl_capped_rows = 0;
    int json_key_candidate_sections = 0;
    int json_key_indexed_sections = 0;
    int json_key_capped_sections = 0;
    int matched_sections = 0;
    int matched_facts = 0;
    int matched_decisions = 0;
    int matched_contracts = 0;
    int matched_runbooks = 0;
    int matched_other_facts = 0;
    int returned_hits = 0;
    int returned_sections = 0;
    int returned_facts = 0;
    std::size_t candidate_rows = 0;
    bool candidate_index_used = false;
    int term_posting_workers = 0;
    long long generated_bytes = 0;
    bool has_build_phase_ms = false;
    BuildPhaseTimings build_phase_ms;
};

struct BenchMetricResult {
    std::string name;
    int iterations = 0;
    std::vector<double> elapsed_ms;
    BenchCounts counts;
};

struct BenchRequest {
    QueryRequest query;
    int iterations = 20;
    int build_iterations = 3;
    int warmup = 2;
};

static BenchRequest parse_bench_request(const std::vector<std::string>& args) {
    BenchRequest request;
    std::vector<std::string> query_parts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--iterations") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--iterations requires a value.");
            }
            request.iterations = parse_cli_integer(args[++i], "--iterations", 1);
        } else if (args[i] == "--build-iterations") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--build-iterations requires a value.");
            }
            request.build_iterations = parse_cli_integer(args[++i], "--build-iterations", 0);
        } else if (args[i] == "--warmup") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--warmup requires a value.");
            }
            request.warmup = parse_cli_integer(args[++i], "--warmup", 0);
        } else if (args[i] == "--limit") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--limit requires a value.");
            }
            request.query.limit = parse_cli_integer(args[++i], "--limit", 1);
        } else if (args[i] == "--per-doc") {
            if (i + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--per-doc requires a value.");
            }
            request.query.per_doc_limit = parse_cli_integer(args[++i], "--per-doc", 1);
        } else {
            query_parts.push_back(args[i]);
        }
    }
    if (request.query.limit == 0) {
        request.query.limit = 8;
    }
    if (request.query.per_doc_limit == 0) {
        request.query.per_doc_limit = 3;
    }
    for (const std::string& part : query_parts) {
        if (!request.query.query.empty()) {
            request.query.query.push_back(' ');
        }
        request.query.query += part;
    }
    request.query.query = trim(request.query.query);
    if (request.query.query.empty()) {
        throw_usage_error("missing-query", "bench requires a non-empty query.");
    }
    request.query.terms = parse_terms(request.query.query);
    request.query.term_groups = expand_query_term_groups(Profile{}, request.query.terms);
    request.query.prepared_term_groups = prepare_term_groups(request.query.term_groups);
    return request;
}

static BenchCounts counts_from_generated_output(const GeneratedOutput& generated, const BuildPhaseTimings* timings = nullptr) {
    BenchCounts counts;
    counts.documents = generated.document_count;
    counts.sections = generated.section_count;
    counts.links = generated.link_count;
    counts.topics = generated.topic_count;
    counts.terms = generated.term_count;
    counts.decisions = generated.decision_count;
    counts.contracts = generated.contract_count;
    counts.runbooks = generated.runbook_count;
    counts.facts = generated.fact_count;
    counts.source_fact_candidate_files = generated.source_fact_candidate_file_count;
    counts.source_fact_scanned_files = generated.source_fact_scanned_file_count;
    counts.source_fact_skipped_large_files = generated.source_fact_skipped_large_file_count;
    counts.source_fact_capped_files = generated.source_fact_capped_file_count;
    counts.json_key_candidate_sections = generated.structured_data.json_key_candidate_sections;
    counts.json_key_indexed_sections = generated.structured_data.json_key_indexed_sections;
    counts.json_key_capped_sections = generated.structured_data.json_key_capped_sections;
    counts.jsonl_candidate_rows = generated.structured_data.jsonl_candidate_rows;
    counts.jsonl_indexed_rows = generated.structured_data.jsonl_indexed_rows;
    counts.jsonl_capped_rows = generated.structured_data.jsonl_capped_rows;
    counts.term_posting_workers = generated.term_posting_workers;
    counts.generated_bytes = generated.generated_bytes;
    if (timings != nullptr) {
        counts.has_build_phase_ms = true;
        counts.build_phase_ms = *timings;
    }
    return counts;
}

static BenchCounts bench_build_no_write_counts(const RuntimeOptions& options) {
    BuildPhaseTimings timings;
    GeneratedOutput generated = generate_outputs(options.profile, options.root, &timings);
    return counts_from_generated_output(generated, &timings);
}

static BenchCounts bench_search_index_counts(const RuntimeOptions& options, const QueryRequest& request) {
    BenchCounts counts;
    const SearchIndexResult result = collect_search_from_index_with_fallback(options, request);
    counts.matched_sections = result.matched_sections;
    counts.matched_facts = result.matched_facts;
    counts.returned_hits = static_cast<int>(result.hits.size());
    counts.candidate_rows = result.diagnostics.candidate_rows;
    counts.candidate_index_used = result.diagnostics.candidate_index_used;
    return counts;
}

static BenchCounts bench_context_index_counts(const RuntimeOptions& options, const QueryRequest& request) {
    BenchCounts counts;
    const ContextIndexResult result = collect_context_from_index_with_fallback(options, request);
    std::vector<ContextHit> selected_hits =
        select_context_sections(result.section_hits_by_doc, request.limit, request.per_doc_limit);
    counts.matched_sections = result.matched_sections;
    counts.matched_facts = result.matched_facts;
    counts.returned_sections = static_cast<int>(selected_hits.size());
    counts.returned_facts = static_cast<int>(result.fact_hits.size());
    counts.candidate_rows = result.diagnostics.candidate_rows;
    counts.candidate_index_used = result.diagnostics.candidate_index_used;
    return counts;
}

static BenchCounts bench_brief_index_counts(const RuntimeOptions& options,
                                            const QueryRequest& request,
                                            const Json& manifest) {
    BenchCounts counts;
    const BriefResult result =
        collect_brief_from_index_with_fallback(options, request, &manifest);
    counts.matched_sections = result.matched_sections;
    counts.matched_facts = result.matched_facts;
    counts.matched_decisions = result.matched_decisions;
    counts.matched_contracts = result.matched_contracts;
    counts.matched_runbooks = result.matched_runbooks;
    counts.matched_other_facts = result.matched_other_facts;
    const BriefSelection selection =
        select_brief_evidence(result, request.limit, request.per_doc_limit);
    counts.returned_sections = static_cast<int>(selection.sections.size());
    counts.returned_facts = static_cast<int>(selection.decisions.size() + selection.contracts.size() +
                                             selection.runbooks.size() + selection.facts.size());
    counts.candidate_rows = result.candidate_rows;
    counts.candidate_index_used = result.candidate_index_used;
    return counts;
}

static BenchMetricResult run_bench_metric(const std::string& name,
                                   int iterations,
                                   int warmup,
                                   const std::function<BenchCounts()>& operation) {
    using Clock = std::chrono::steady_clock;
    BenchMetricResult result;
    result.name = name;
    result.iterations = iterations;
    for (int i = 0; i < warmup; ++i) {
        result.counts = operation();
    }
    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        result.counts = operation();
        const auto end = Clock::now();
        const std::chrono::duration<double, std::milli> elapsed = end - start;
        result.elapsed_ms.push_back(elapsed.count());
    }
    return result;
}

static double percentile_ms(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double raw_index = percentile * static_cast<double>(values.size() - 1);
    const std::size_t index = static_cast<std::size_t>(std::lround(raw_index));
    return values[std::min(index, values.size() - 1)];
}

static double average_ms(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return total / static_cast<double>(values.size());
}

static std::string json_ms(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

static void emit_build_phase_timings_json(std::ostream& out, const BuildPhaseTimings& timings) {
    out << "{"
        << "\"read_sources\":" << json_ms(timings.read_sources_ms)
        << ",\"document_rows\":" << json_ms(timings.document_rows_ms)
        << ",\"source_facts\":" << json_ms(timings.source_facts_ms)
        << ",\"authority_rows\":" << json_ms(timings.authority_rows_ms)
        << ",\"sort_rows\":" << json_ms(timings.sort_rows_ms)
        << ",\"final_counts\":" << json_ms(timings.final_counts_ms)
        << ",\"term_postings\":" << json_ms(timings.term_postings_ms)
        << ",\"term_render\":" << json_ms(timings.term_render_ms)
        << ",\"row_index_render\":" << json_ms(timings.row_index_render_ms)
        << ",\"json_render\":" << json_ms(timings.json_render_ms)
        << ",\"total_generate\":" << json_ms(timings.total_generate_ms)
        << "}";
}

static void emit_bench_counts_json(std::ostream& out, const BenchCounts& counts) {
    out << "{"
        << "\"documents\":" << counts.documents
        << ",\"sections\":" << counts.sections
        << ",\"links\":" << counts.links
        << ",\"topics\":" << counts.topics
        << ",\"terms\":" << counts.terms
        << ",\"decisions\":" << counts.decisions
        << ",\"contracts\":" << counts.contracts
        << ",\"runbooks\":" << counts.runbooks
        << ",\"facts\":" << counts.facts
        << ",\"source_fact_candidate_files\":" << counts.source_fact_candidate_files
        << ",\"source_fact_scanned_files\":" << counts.source_fact_scanned_files
        << ",\"source_fact_skipped_large_files\":" << counts.source_fact_skipped_large_files
        << ",\"source_fact_capped_files\":" << counts.source_fact_capped_files
        << ",\"json_key_candidate_sections\":" << counts.json_key_candidate_sections
        << ",\"json_key_indexed_sections\":" << counts.json_key_indexed_sections
        << ",\"json_key_capped_sections\":" << counts.json_key_capped_sections
        << ",\"jsonl_candidate_rows\":" << counts.jsonl_candidate_rows
        << ",\"jsonl_indexed_rows\":" << counts.jsonl_indexed_rows
        << ",\"jsonl_capped_rows\":" << counts.jsonl_capped_rows
        << ",\"matched_sections\":" << counts.matched_sections
        << ",\"matched_facts\":" << counts.matched_facts
        << ",\"matched_decisions\":" << counts.matched_decisions
        << ",\"matched_contracts\":" << counts.matched_contracts
        << ",\"matched_runbooks\":" << counts.matched_runbooks
        << ",\"matched_other_facts\":" << counts.matched_other_facts
        << ",\"returned_hits\":" << counts.returned_hits
        << ",\"returned_sections\":" << counts.returned_sections
        << ",\"returned_facts\":" << counts.returned_facts
        << ",\"candidate_rows\":" << counts.candidate_rows
        << ",\"candidate_index_used\":" << (counts.candidate_index_used ? "true" : "false")
        << ",\"generated_bytes\":" << counts.generated_bytes;
    if (counts.has_build_phase_ms) {
        out << ",\"term_posting_workers\":" << counts.term_posting_workers
            << ",\"build_phase_ms\":";
        emit_build_phase_timings_json(out, counts.build_phase_ms);
        out << ",\"build_memory_scope\":\"process-high-water-mark-at-phase-boundary\""
            << ",\"build_phase_peak_rss_bytes\":{";
        bool first = true;
        for (const auto& [phase, bytes] : counts.build_phase_ms.peak_rss_bytes) {
            if (!first) out << ',';
            first = false;
            out << json_string(phase) << ':';
            if (bytes == 0) out << "null";
            else out << bytes;
        }
        out << '}';
    }
    out << "}";
}

static void emit_bench_metric_json(std::ostream& out, const BenchMetricResult& metric) {
    const std::vector<double>& values = metric.elapsed_ms;
    const double min_value = values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
    const double max_value = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    out << "{"
        << "\"name\":" << json_string(metric.name)
        << ",\"iterations\":" << metric.iterations
        << ",\"latency_ms\":{"
        << "\"min\":" << json_ms(min_value)
        << ",\"p50\":" << json_ms(percentile_ms(values, 0.50))
        << ",\"p95\":" << json_ms(percentile_ms(values, 0.95))
        << ",\"max\":" << json_ms(max_value)
        << ",\"avg\":" << json_ms(average_ms(values))
        << "},\"counts\":";
    emit_bench_counts_json(out, metric.counts);
    out << "}";
}

}  // namespace

int run_bench(const RuntimeOptions& options, const std::vector<std::string>& args) {
    BenchRequest request = parse_bench_request(args);
    apply_profile_query_expansion(options.profile, request.query);
    if (!generated_index_available(options)) {
        throw_index_error("index-unavailable", "bench requires a complete generated index.");
    }
    RetrievalSession session(options, false);
    const RuntimeOptions& session_options = session.options();

    std::vector<BenchMetricResult> metrics;
    if (request.build_iterations > 0) {
        metrics.push_back(run_bench_metric("build_no_write", request.build_iterations, 0, [&]() {
            return bench_build_no_write_counts(options);
        }));
    }
    metrics.push_back(run_bench_metric("search_generated_index", request.iterations, request.warmup, [&]() {
        return bench_search_index_counts(session_options, request.query);
    }));
    metrics.push_back(run_bench_metric("context_generated_index", request.iterations, request.warmup, [&]() {
        return bench_context_index_counts(session_options, request.query);
    }));
    metrics.push_back(run_bench_metric("brief_generated_index", request.iterations, request.warmup, [&]() {
        return bench_brief_index_counts(session_options, request.query, session.manifest());
    }));

    std::cout << "{\n"
              << "  \"schema_version\": \"shin.perf.v1\",\n"
              << "  \"tool\": \"shin-context\",\n"
              << "  \"command\": \"bench\",\n"
              << "  \"ok\": true,\n"
              << "  \"profile\": " << json_string(options.profile.id) << ",\n"
              << "  \"root\": " << json_string(options.root.generic_string()) << ",\n"
              << "  \"out_root\": " << json_string(options.out_root.generic_string()) << ",\n"
              << "  \"query\": " << json_string(request.query.query) << ",\n"
              << "  \"iterations\": " << request.iterations << ",\n"
              << "  \"build_iterations\": " << request.build_iterations << ",\n"
              << "  \"warmup\": " << request.warmup << ",\n"
              << "  \"limit\": " << request.query.limit << ",\n"
              << "  \"per_doc_limit\": " << request.query.per_doc_limit << ",\n"
              << "  \"cpu_threads_detected\": " << std::thread::hardware_concurrency() << ",\n"
              << "  \"generated_index_source_count\": " << generated_index_source_count(session_options) << ",\n"
              << "  \"metrics\": [\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        std::cout << "    ";
        emit_bench_metric_json(std::cout, metrics[i]);
        if (i + 1 < metrics.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
    return 0;
}

} // namespace shin::context
