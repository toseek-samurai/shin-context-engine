#include "agent_context_pack.h"
#include "context_internal.h"

#include <numeric>

namespace shin::context {
namespace {

struct PackBenchOptions {
    fs::path input;
    bool stdin_input = false;
    bool require_current = false;
    bool strict = false;
    PackProjection projection = PackProjection::compact;
    int jobs = 0;
    int iterations = 20;
    int warmup = 2;
};

struct PackBenchSample {
    double session_open_ms = 0.0;
    double retrieval_ms = 0.0;
    double selection_ms = 0.0;
    double projection_ms = 0.0;
    double serialization_ms = 0.0;
    double total_ms = 0.0;
    std::size_t output_bytes = 0;
    std::size_t evidence_bytes = 0;
    std::size_t evidence_records = 0;
    std::string receipt_id;
    std::string evidence_set_fingerprint;
    std::string snapshot_id;
    unsigned int jobs = 1;
    bool complete = false;
};

PackBenchOptions parse_pack_bench_options(const std::vector<std::string>& args) {
    PackBenchOptions options;
    bool input_seen = false;
    bool projection_seen = false;
    bool jobs_seen = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        if (argument == "--input") {
            if (input_seen || index + 1 >= args.size()) {
                throw_usage_error(input_seen ? "duplicate-option" : "missing-option-value",
                                  input_seen ? "--input may be supplied only once."
                                             : "--input requires a file path or -.");
            }
            const std::string& value = args[++index];
            options.stdin_input = value == "-";
            if (!options.stdin_input) {
                options.input = fs::absolute(value);
            }
            input_seen = true;
        } else if (argument == "--projection") {
            if (projection_seen || index + 1 >= args.size()) {
                throw_usage_error(projection_seen ? "duplicate-option" : "missing-option-value",
                                  projection_seen ? "--projection may be supplied only once."
                                                  : "--projection requires full or compact.");
            }
            const std::string& value = args[++index];
            if (value == "full") {
                options.projection = PackProjection::full;
            } else if (value == "compact") {
                options.projection = PackProjection::compact;
            } else {
                throw_usage_error("invalid-option-value",
                                  "--projection requires full or compact.");
            }
            projection_seen = true;
        } else if (argument == "--jobs") {
            if (jobs_seen || index + 1 >= args.size()) {
                throw_usage_error(jobs_seen ? "duplicate-option" : "missing-option-value",
                                  jobs_seen ? "--jobs may be supplied only once."
                                            : "--jobs requires a value.");
            }
            options.jobs = parse_cli_integer(args[++index], "--jobs", 1);
            if (options.jobs > kMaxPackJobs) {
                throw_usage_error("invalid-option-value", "--jobs must not exceed 8.");
            }
            jobs_seen = true;
        } else if (argument == "--iterations") {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--iterations requires a value.");
            }
            options.iterations = parse_cli_integer(args[++index], "--iterations", 1);
            if (options.iterations > 1000) {
                throw_usage_error("invalid-option-value", "--iterations must not exceed 1000.");
            }
        } else if (argument == "--warmup") {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", "--warmup requires a value.");
            }
            options.warmup = parse_cli_integer(args[++index], "--warmup", 0);
            if (options.warmup > 100) {
                throw_usage_error("invalid-option-value", "--warmup must not exceed 100.");
            }
        } else if (argument == "--require-current") {
            options.require_current = true;
        } else if (argument == "--strict") {
            options.strict = true;
        } else {
            throw_usage_error(
                "unsupported-option",
                "pack-bench only accepts --input, --projection, --jobs, --iterations, "
                "--warmup, --require-current, and --strict.");
        }
    }
    if (!input_seen) {
        throw_usage_error("missing-option", "pack-bench requires --input FILE or --input -.");
    }
    return options;
}

PackBenchSample run_pack_bench_sample(const RuntimeOptions& options,
                                      const PackBenchOptions& bench_options,
                                      const TaskRequest& request) {
    PackBenchSample sample;
    const SteadyClock::time_point total_start = SteadyClock::now();
    const SteadyClock::time_point session_start = SteadyClock::now();
    RetrievalSession session(options, bench_options.require_current);
    sample.session_open_ms = elapsed_ms_since(session_start);

    const AgentContextPack pack = execute_agent_context_pack(
        options, request, session, bench_options.jobs);
    sample.retrieval_ms = pack.retrieval_ms;
    sample.selection_ms = pack.selection_ms;
    sample.evidence_bytes = pack.evidence_bytes;
    sample.evidence_records = pack.evidence_ids.size();
    sample.receipt_id = pack.receipt_id;
    sample.evidence_set_fingerprint = pack.evidence_set_fingerprint;
    sample.snapshot_id = session.freshness().snapshot_id;
    sample.jobs = pack.jobs;
    sample.complete = pack.completion.complete;

    const PackRenderOptions render_options = {
        bench_options.projection,
        bench_options.require_current,
        bench_options.strict
    };
    const SteadyClock::time_point projection_start = SteadyClock::now();
    const Json output = render_agent_context_pack(
        options, render_options, request, session, pack);
    sample.projection_ms = elapsed_ms_since(projection_start);
    const SteadyClock::time_point serialization_start = SteadyClock::now();
    sample.output_bytes = output.dump(2).size();
    sample.serialization_ms = elapsed_ms_since(serialization_start);
    sample.total_ms = elapsed_ms_since(total_start);
    return sample;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double raw_index = fraction * static_cast<double>(values.size() - 1);
    const std::size_t index = static_cast<std::size_t>(std::lround(raw_index));
    return values[std::min(index, values.size() - 1)];
}

Json latency_distribution(const std::vector<double>& values) {
    const double minimum = values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
    const double maximum = values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
    const double total = std::accumulate(values.begin(), values.end(), 0.0);
    auto rounded = [](double value) {
        return std::round(value * 1000.0) / 1000.0;
    };
    return {
        {"min", rounded(minimum)},
        {"p50", rounded(percentile(values, 0.50))},
        {"p95", rounded(percentile(values, 0.95))},
        {"max", rounded(maximum)},
        {"avg", rounded(values.empty() ? 0.0 : total / static_cast<double>(values.size()))}
    };
}

template <typename Value>
Json integral_distribution(const std::vector<Value>& values) {
    if (values.empty()) {
        return {{"min", 0}, {"p50", 0}, {"p95", 0}, {"max", 0}};
    }
    std::vector<Value> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    auto selected = [&](double fraction) {
        const double raw_index = fraction * static_cast<double>(sorted.size() - 1);
        const std::size_t index = static_cast<std::size_t>(std::lround(raw_index));
        return sorted[std::min(index, sorted.size() - 1)];
    };
    return {
        {"min", sorted.front()},
        {"p50", selected(0.50)},
        {"p95", selected(0.95)},
        {"max", sorted.back()}
    };
}

}  // namespace

int run_pack_bench(const RuntimeOptions& options, const std::vector<std::string>& args) {
    const PackBenchOptions bench_options = parse_pack_bench_options(args);
    const Json input = read_task_request_document(bench_options.input,
                                                  bench_options.stdin_input);
    const TaskRequest request = parse_task_request(input, options.profile);

    for (int index = 0; index < bench_options.warmup; ++index) {
        (void)run_pack_bench_sample(options, bench_options, request);
    }

    std::vector<PackBenchSample> samples;
    samples.reserve(static_cast<std::size_t>(bench_options.iterations));
    for (int index = 0; index < bench_options.iterations; ++index) {
        samples.push_back(run_pack_bench_sample(options, bench_options, request));
    }

    const PackBenchSample& baseline = samples.front();
    const bool receipt_stable = std::all_of(
        samples.begin(), samples.end(), [&](const PackBenchSample& sample) {
            return sample.receipt_id == baseline.receipt_id &&
                   sample.evidence_set_fingerprint == baseline.evidence_set_fingerprint &&
                   sample.snapshot_id == baseline.snapshot_id;
        });
    const bool completion_stable = std::all_of(
        samples.begin(), samples.end(), [&](const PackBenchSample& sample) {
            return sample.complete == baseline.complete;
        });
    const bool completion_acceptable = !bench_options.strict || baseline.complete;
    const bool ok = receipt_stable && completion_stable && completion_acceptable;

    auto latency_values = [&](auto member) {
        std::vector<double> values;
        values.reserve(samples.size());
        for (const PackBenchSample& sample : samples) {
            values.push_back(sample.*member);
        }
        return values;
    };
    std::vector<std::size_t> output_bytes;
    output_bytes.reserve(samples.size());
    for (const PackBenchSample& sample : samples) {
        output_bytes.push_back(sample.output_bytes);
    }

    Json output = make_result_document("shin.pack-bench.v1", "pack-bench", ok);
    output["profile"] = options.profile.id;
    output["root"] = options.root.generic_string();
    output["out_root"] = options.out_root.generic_string();
    output["request_id"] = request.request_id;
    output["projection"] = pack_projection_name(bench_options.projection);
    output["iterations"] = bench_options.iterations;
    output["warmup"] = bench_options.warmup;
    output["jobs"] = baseline.jobs;
    output["require_current"] = bench_options.require_current;
    output["strict"] = bench_options.strict;
    output["snapshot_id"] = baseline.snapshot_id;
    output["receipt_stable"] = receipt_stable;
    output["complete"] = baseline.complete;
    output["completion_stable"] = completion_stable;
    output["receipt_id"] = baseline.receipt_id;
    output["evidence_set_fingerprint"] = baseline.evidence_set_fingerprint;
    output["evidence_bytes"] = baseline.evidence_bytes;
    output["evidence_records"] = baseline.evidence_records;
    output["process_peak_rss_bytes"] = process_peak_rss_bytes();
    output["phase_latency_ms"] = {
        {"session_open", latency_distribution(latency_values(&PackBenchSample::session_open_ms))},
        {"retrieval", latency_distribution(latency_values(&PackBenchSample::retrieval_ms))},
        {"selection", latency_distribution(latency_values(&PackBenchSample::selection_ms))},
        {"projection", latency_distribution(latency_values(&PackBenchSample::projection_ms))},
        {"serialization", latency_distribution(latency_values(&PackBenchSample::serialization_ms))},
        {"total", latency_distribution(latency_values(&PackBenchSample::total_ms))}
    };
    output["output_bytes"] = integral_distribution(output_bytes);
    emit_json_document(std::cout, output);
    return ok ? 0 : 1;
}

}  // namespace shin::context
