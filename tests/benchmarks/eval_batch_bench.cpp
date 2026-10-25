#include "context_internal.h"
#include "profile_quality.h"
#include "eval_batch_experiment.h"
#include "eval_batch_route_probe.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace shin::context;
using namespace shin::context::experiment;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json suite_summary(const SuiteEvaluation& suite) {
    return {{"total", suite.total}, {"passed", suite.passed}, {"failed", suite.failed},
        {"queries", suite.query_total}, {"queries_passed", suite.query_passed},
        {"quality_failed_queries", suite.quality_failed_queries},
        {"resource_failed_queries", suite.resource_failed_queries},
        {"failed_case_ids", suite.failed_case_ids}};
}

BriefTask ready_task(int value) {
    BriefResult result;
    result.matched_sections = value;
    co_return result;
}

BriefTask failing_task() {
    throw std::runtime_error("expected coroutine probe failure");
    co_return BriefResult{};
}

BriefTask suspended_task(const RuntimeOptions& options, const QueryRequest& request) {
    StageAwaiter stage(options, request, CandidateRows{}, 0, false);
    BriefResult result = co_await stage;
    co_return result;
}

void synthetic_lifecycle_probe() {
    RuntimeOptions options;
    QueryRequest request;

    auto original = ready_task(17);
    require(!original.done(), "coroutine did not start suspended");
    BriefTask moved(std::move(original));
    require(original.done() && !moved.done(), "coroutine move did not transfer ownership");
    moved.handle().resume();
    require(moved.done(), "ready coroutine did not complete");
    require(moved.take_result().matched_sections == 17, "ready coroutine result was lost");

    auto pending = suspended_task(options, request);
    const auto pending_handle = pending.handle();
    pending_handle.resume();
    require(!pending.done() && pending_handle.promise().pending != nullptr,
            "awaited stage was not registered");
    pending_handle.promise().pending->set_result(BriefResult{});
    pending_handle.resume();
    require(pending.done(), "resumed stage coroutine did not complete");
    (void)pending.take_result();

    auto cancelled = suspended_task(options, request);
    cancelled.handle().resume();
    require(cancelled.handle().promise().pending != nullptr,
            "cancellation probe did not reach its pending stage");
    cancelled = BriefTask{};

    auto failed = failing_task();
    failed.handle().resume();
    require(failed.done(), "throwing coroutine did not complete");
    bool propagated = false;
    try {
        (void)failed.take_result();
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "expected coroutine probe failure";
    }
    require(propagated, "coroutine exception was not preserved");
    // Exercise the actual scheduler's stored-error path, with no filesystem writes.
    options.out_root = fs::temp_directory_path() /
        ("shin-e2d-absent-" + std::to_string(SteadyClock::now().time_since_epoch().count()));
    require(!fs::exists(options.out_root), "missing-index probe path unexpectedly exists");
    options.root = options.out_root;
    request.query = "neutralmissing";
    request.terms = parse_terms(request.query);
    request.limit = 8; request.per_doc_limit = 2;
    apply_profile_query_expansion(options.profile, request);
    const std::vector<QueryRequest> broken{request, request};
    auto failure = [&](bool batch) {
        try { (void)run_brief_tasks(options, broken, nullptr, batch); }
        catch (const std::exception& error) { return std::string(error.what()); }
        return std::string{};
    };
    configure_eval_batch(false, 8);
    const auto serial_error = failure(false);
    configure_eval_batch(true, 8);
    const auto batch_error = failure(true);
    require(!serial_error.empty() && serial_error == batch_error, "scheduler changed missing-index error");
    require(batch_metrics()["error_replays"] == 1 && batch_metrics()["pending_count"] == 0,
            "stored coroutine error bypassed serial replay or leaked pending state");
    configure_eval_batch(false, 4);

}

std::vector<QueryRequest> bounded_brief_requests(const Profile& profile) {
    constexpr std::size_t kMaxRequests = 256;
    std::vector<QueryRequest> requests;
    for (const auto& item : profile.eval_cases) {
        auto append = [&](const std::string& query) {
            require(requests.size() < kMaxRequests,
                    "profile exceeds the 256-query brief-parity bound");
            QueryRequest request;
            request.query = query;
            request.terms = parse_terms(query);
            request.limit = item.limit > 0 ? item.limit : 8;
            request.per_doc_limit = item.per_doc_limit > 0 ? item.per_doc_limit : 2;
            apply_profile_query_expansion(profile, request);
            requests.push_back(std::move(request));
        };
        append(item.query);
        for (const auto& variant : item.query_variants) append(variant);
    }
    return requests;
}

std::size_t profile_query_count(const Profile& profile) {
    std::size_t count = 0;
    for (const auto& item : profile.eval_cases) {
        count += 1 + item.query_variants.size();
        require(count <= 256, "profile exceeds the 256-query benchmark bound");
    }
    return count;
}

int benchmark(int argc, char** argv) {
    synthetic_lifecycle_probe();
    if (argc == 1) {
        std::cout << Json({{"ok", true}, {"synthetic_lifecycle_probe", true}}).dump(2) << '\n';
        return 0;
    }
    if (argc != 7 && argc != 8) {
        std::cerr << "usage: shin-eval-batch-bench ROOT OUTPUT PROFILE_DIR PROFILE baseline-first|candidate-first BATCH_SIZE [ORACLE_JSONL]\n";
        return 2;
    }

    const std::string order = argv[5];
    require(order == "baseline-first" || order == "candidate-first", "invalid lane order");
    std::size_t parsed_chars = 0;
    const auto parsed_batch_size = std::stoull(argv[6], &parsed_chars);
    require(parsed_chars == std::string(argv[6]).size(), "batch size must be an integer");
    const std::size_t batch_size = static_cast<std::size_t>(parsed_batch_size);
    require(batch_size >= 1, "batch size must be at least 1");
    require(batch_size <= 8, "batch size exceeds the experiment cap of 8");

    const bool external_oracle_checked = argc == 8;
    std::ifstream oracle_stream;
    if (external_oracle_checked) {
        oracle_stream.open(argv[7]);
        require(oracle_stream.is_open(), "could not open external oracle JSONL");
    }
    auto read_oracle_json = [&]() {
        std::string line;
        require(static_cast<bool>(std::getline(oracle_stream, line)),
                "external oracle JSONL ended before all expected rows");
        require(!line.empty(), "external oracle JSONL contains an empty row");
        return Json::parse(line);
    };

    RuntimeOptions options;
    options.root = fs::canonical(argv[1]);
    options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]);
    options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    const auto expected_query_count = profile_query_count(options.profile);
    require(expected_query_count > 0, "profile has no eval queries");

    const auto admission_start = SteadyClock::now();
    RetrievalSession session(options, RetrievalSessionOpenMode::require_current);
    Json result{{"schema_version", "shin.eval-batch-experiment.v1"},
        {"order", order}, {"batch_size", batch_size},
        {"profile", options.profile.id}, {"eval_queries", expected_query_count},
        {"admission_ms", elapsed_ms_since(admission_start)}, {"runs", Json::array()}};

    Json oracle_summary;
    for (bool candidate : {order == "candidate-first", order != "candidate-first"}) {
        configure_eval_batch(candidate, batch_size);
        const auto wall_start = SteadyClock::now();
        const auto cpu_start = std::clock();
        const auto suite = evaluate_profile_cases(session);
        const auto wall_ms = elapsed_ms_since(wall_start);
        const auto cpu_end = std::clock();
        const auto rss_after_lane = process_peak_rss_bytes();
        const auto summary = suite_summary(suite);
        if (oracle_summary.is_null()) oracle_summary = summary;
        else require(summary == oracle_summary, "baseline and candidate eval summaries differ");

        result["runs"].push_back({{"candidate", candidate}, {"wall_ms", wall_ms},
            {"process_cpu_ms", cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1)
                ? Json(nullptr) : Json(1000.0 * (cpu_end - cpu_start) / CLOCKS_PER_SEC)},
            {"process_peak_rss_after_lane_bytes", rss_after_lane}, {"summary", summary},
            {"batch_metrics", batch_metrics()}});
    }

    // Full diagnostics and results are compared outside timing. The baseline uses
    // the ordinary session path; the candidate uses the real batch coroutine path.
    configure_eval_batch(false, batch_size);
    const auto baseline_details = eval_details(session);
    configure_eval_batch(true, batch_size);
    const auto candidate_details = eval_details(session);
    require(baseline_details == candidate_details, "full eval case parity failed");
    require(baseline_details.is_array() && baseline_details.size() == options.profile.eval_cases.size(),
            "full eval detail count does not match the profile base cases");
    std::size_t detailed_query_count = baseline_details.size();
    for (const auto& detail : baseline_details) {
        require(detail.is_object() && detail.contains("query_variants") &&
                    detail["query_variants"].is_array(),
                "full eval detail is missing its variant results");
        detailed_query_count += detail["query_variants"].size();
    }
    require(detailed_query_count == expected_query_count,
            "full eval query count does not match the profile base and variant queries");
    result["exact_eval_cases"] = baseline_details.size();
    result["exact_eval_queries"] = detailed_query_count;

    const auto brief_requests = bounded_brief_requests(options.profile);
    require(brief_requests.size() == expected_query_count,
            "brief request count does not match the profile queries");
    for (std::size_t first = 0; first < brief_requests.size(); first += batch_size) {
        const auto last = std::min(first + batch_size, brief_requests.size());
        const std::vector<QueryRequest> slice(brief_requests.begin() + first,
                                              brief_requests.begin() + last);
        configure_eval_batch(false, batch_size);
        Json baseline_briefs = Json::array();
        for (const auto& request : slice)
            baseline_briefs.push_back(session.brief(request, BriefProjection::full));
        configure_eval_batch(true, batch_size);
        const auto candidate_briefs = batch_brief_documents(session, slice);
        require(baseline_briefs == candidate_briefs, "full brief slice parity failed");
        if (external_oracle_checked) {
            for (const auto& baseline_brief : baseline_briefs)
                require(read_oracle_json() == baseline_brief,
                        "baseline brief differs from the external Core oracle");
        }
    }
    result["exact_brief_queries"] = brief_requests.size();

    Json external_eval_summary;
    Json external_pack_summary;
    if (external_oracle_checked) {
        const Json final_oracle_row = read_oracle_json();
        require(final_oracle_row.is_object() && final_oracle_row.contains("eval") &&
                    final_oracle_row.contains("pack"),
                "external oracle final row must contain eval and pack summaries");
        external_eval_summary = final_oracle_row["eval"];
        external_pack_summary = final_oracle_row["pack"];
        require(external_eval_summary == oracle_summary,
                "eval summary differs from the external Core oracle");
        std::string trailing_line;
        while (std::getline(oracle_stream, trailing_line)) {
            require(trailing_line.find_first_not_of(" \t\r\n") == std::string::npos,
                    "external oracle JSONL has rows after its final summary");
        }
    }

    configure_eval_batch(false, batch_size);
    const auto baseline_pack_summary = suite_summary(evaluate_pack_cases(session));
    configure_eval_batch(true, batch_size);
    const auto candidate_pack_summary = suite_summary(evaluate_pack_cases(session));
    require(baseline_pack_summary == candidate_pack_summary,
            "baseline and candidate Pack summaries differ");
    if (external_oracle_checked)
        require(baseline_pack_summary == external_pack_summary,
                "Pack summary differs from the external Core oracle");
    result["pack_summary"] = baseline_pack_summary;
    result["pack_parity"] = true;
    result["external_oracle_checked"] = external_oracle_checked;
    result["untimed_route_probe"] = Json::array();
    for (const bool candidate : {false, true}) {
        configure_eval_batch(candidate, batch_size);
        configure_route_probe(true);
        const auto probe_suite = evaluate_profile_cases(session);
        const auto measured = route_probe_metrics();
        configure_route_probe(false);
        require(suite_summary(probe_suite) == oracle_summary, "route probe changed eval summary");
        result["untimed_route_probe"].push_back({{"candidate", candidate}, {"routes", measured}});
    }
    result["group_cutoff_probe"] = Json::array();
    for (const auto& request : brief_requests) {
        if (strict_required_group_matches(request) < kCompoundQueryCoverageThreshold) continue;
        const auto groups = load_generated_query_group_candidates(session.options(), request.prepared_term_groups);
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].from_terms_index && groups[i].term_index_coverage_complete) continue;
            result["group_cutoff_probe"].push_back({{"query", request.query}, {"group", i},
                {"terms", request.term_groups.at(i)}, {"discovered_rows", groups[i].discovered_row_count},
                {"coverage_complete", groups[i].term_index_coverage_complete}});
        }
    }
    result["summary"] = oracle_summary;
    result["quality_passed"] = oracle_summary["total"].get<std::size_t>() > 0 &&
                                oracle_summary["failed"].get<std::size_t>() == 0;
    result["ok"] = true;
    result["process_peak_rss_bytes"] = process_peak_rss_bytes();
    result["boundary"] = "Optional benchmark only; one strict current RetrievalSession is shared by both lanes. Timed eval includes the complete typed profile evaluation and its assertions, but excludes admission, exact parity passes and Pack parity. Full eval details are compared outside timing; every base and variant brief is compared in slices no larger than the configured batch size, and an optional normal-Core JSONL oracle is streamed row by row. Peak RSS is process high-water RSS sampled after each eval lane. No installation or production speedup claim.";
    std::cout << result.dump(2) << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    try { return benchmark(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
