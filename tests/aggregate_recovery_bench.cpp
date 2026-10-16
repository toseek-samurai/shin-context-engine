#include "context_internal.h"
#include "profile_quality.h"
#include "aggregate_recovery_experiment.h"

#include <ctime>
#include <functional>
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

Json normalized_brief(Json brief) {
    require(brief.is_object() && brief.contains("query_diagnostics") &&
                brief["query_diagnostics"].is_object(),
            "full brief is missing query_diagnostics");
    auto& diagnostics = brief["query_diagnostics"];
    diagnostics.erase("candidate_rows");
    diagnostics.erase("candidate_index_used");
    return brief;
}

Json diagnostic_value(const Json& brief, const char* key) {
    const auto it = brief.find("query_diagnostics");
    if (it == brief.end() || !it->is_object()) return nullptr;
    const auto value = it->find(key);
    return value == it->end() ? Json(nullptr) : *value;
}

void require_healthy(const Json& summary, const char* label) {
    require(summary.is_object() && summary.contains("total") && summary.contains("failed") &&
                summary.contains("resource_failed_queries") && summary.contains("quality_failed_queries"),
            label);
    require(summary["total"].get<std::size_t>() > 0, label);
    require(summary["failed"].get<std::size_t>() == 0, label);
    require(summary["passed"] == summary["total"], label);
    require(summary["queries_passed"] == summary["queries"], label);
    require(summary["resource_failed_queries"].get<std::size_t>() == 0, label);
    require(summary["quality_failed_queries"].get<std::size_t>() == 0, label);
}

Json run_timed_suite(bool candidate, const char* suite_name,
                     const std::function<SuiteEvaluation()>& evaluate) {
    configure_aggregate_recovery(candidate);
    const auto wall_start = SteadyClock::now();
    const auto cpu_start = std::clock();
    const auto suite = evaluate();
    const auto wall_ms = elapsed_ms_since(wall_start);
    const auto cpu_end = std::clock();
    const auto rss = process_peak_rss_bytes();
    const Json summary = suite_summary(suite);
    return {{"suite", suite_name}, {"candidate", candidate}, {"wall_ms", wall_ms},
        {"process_cpu_ms", cpu_start == std::clock_t(-1) || cpu_end == std::clock_t(-1)
            ? Json(nullptr) : Json(1000.0 * (cpu_end - cpu_start) / CLOCKS_PER_SEC)},
        {"process_peak_rss_after_lane_bytes", rss}, {"summary", summary},
        {"aggregate_metrics", aggregate_metrics()}};
}

int benchmark(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        std::cerr << "usage: shin-aggregate-recovery-bench ROOT OUT PROFILE_DIR PROFILE baseline-first|candidate-first [ORACLE_JSONL]\n";
        return 2;
    }
    const std::string order = argv[5];
    require(order == "baseline-first" || order == "candidate-first", "invalid lane order");

    const bool external_oracle_checked = argc == 7;
    std::ifstream oracle_stream;
    if (external_oracle_checked) {
        oracle_stream.open(argv[6]);
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
    const auto brief_requests = bounded_brief_requests(options.profile);
    require(brief_requests.size() == expected_query_count,
            "brief request count does not match profile base and variant queries");

    const auto admission_start = SteadyClock::now();
    RetrievalSession session(options, RetrievalSessionOpenMode::require_current);
    Json result{{"schema_version", "shin.aggregate-recovery-experiment.v1"},
        {"order", order}, {"profile", options.profile.id}, {"eval_queries", expected_query_count},
        {"admission_ms", elapsed_ms_since(admission_start)}, {"runs", Json::array()}};

    Json eval_summary;
    Json pack_summary;
    const bool first_candidate = order == "candidate-first";
    for (const bool candidate : {first_candidate, !first_candidate}) {
        const auto eval_run = run_timed_suite(candidate, "eval", [&] {
            return evaluate_profile_cases(session);
        });
        const auto current_eval = eval_run["summary"];
        if (current_eval["failed"] != 0) std::cerr << eval_run.dump() << '\n';
        require_healthy(current_eval, "eval lane did not pass all quality and resource assertions");
        require(current_eval["queries"].get<std::size_t>() == expected_query_count,
                "eval lane query count does not match profile base and variant queries");
        if (eval_summary.is_null()) eval_summary = current_eval;
        else require(current_eval == eval_summary, "baseline and candidate eval summaries differ");
        result["runs"].push_back(eval_run);

        const auto pack_run = run_timed_suite(candidate, "pack", [&] {
            return evaluate_pack_cases(session);
        });
        const auto current_pack = pack_run["summary"];
        if (current_pack["failed"] != 0) std::cerr << pack_run.dump() << '\n';
        require_healthy(current_pack, "Pack lane did not pass all quality and resource assertions");
        if (pack_summary.is_null()) pack_summary = current_pack;
        else require(current_pack == pack_summary, "baseline and candidate Pack summaries differ");
        result["runs"].push_back(pack_run);
    }

    // Compare every full brief one at a time outside timing. Only the two fields
    // deliberately changed by aggregate recovery are excluded from exact parity.
    configure_aggregate_recovery(false);
    Json diagnostic_deltas = Json::array();
    for (const auto& request : brief_requests) {
        const Json baseline = session.brief(request, BriefProjection::full);
        if (external_oracle_checked)
            require(read_oracle_json() == baseline, "baseline brief differs from normal-Core oracle");
        configure_aggregate_recovery(true);
        const Json candidate = session.brief(request, BriefProjection::full);
        require(normalized_brief(baseline) == normalized_brief(candidate),
                "full brief parity failed outside candidate recovery diagnostics");
        const Json baseline_rows = diagnostic_value(baseline, "candidate_rows");
        const Json candidate_rows = diagnostic_value(candidate, "candidate_rows");
        const Json baseline_index = diagnostic_value(baseline, "candidate_index_used");
        const Json candidate_index = diagnostic_value(candidate, "candidate_index_used");
        diagnostic_deltas.push_back({{"query", request.query},
            {"candidate_rows", {{"baseline", baseline_rows}, {"candidate", candidate_rows},
                                {"changed", baseline_rows != candidate_rows}}},
            {"candidate_index_used", {{"baseline", baseline_index}, {"candidate", candidate_index},
                                      {"changed", baseline_index != candidate_index}}}});
        configure_aggregate_recovery(false);
    }
    result["full_brief_parity"] = true;
    result["full_brief_queries"] = brief_requests.size();
    result["diagnostic_deltas"] = std::move(diagnostic_deltas);

    if (external_oracle_checked) {
        const Json final_oracle_row = read_oracle_json();
        require(final_oracle_row.is_object() && final_oracle_row.contains("eval") &&
                    final_oracle_row.contains("pack"),
                "external oracle final row must contain eval and pack summaries");
        require(final_oracle_row["eval"] == eval_summary,
                "eval summary differs from normal-Core oracle");
        require(final_oracle_row["pack"] == pack_summary,
                "Pack summary differs from normal-Core oracle");
        std::string trailing_line;
        while (std::getline(oracle_stream, trailing_line))
            require(trailing_line.find_first_not_of(" \t\r\n") == std::string::npos,
                    "external oracle JSONL has rows after its final summary");
    }

    result["eval_summary"] = eval_summary;
    result["pack_summary"] = pack_summary;
    result["external_oracle_checked"] = external_oracle_checked;
    result["quality_passed"] = true;
    result["ok"] = true;
    result["process_peak_rss_bytes"] = process_peak_rss_bytes();
    result["boundary"] = "Optional aggregate-recovery benchmark only; one strict current RetrievalSession is shared by baseline and candidate. Eval and Pack are timed separately with full native assertions, excluding admission, oracle comparison and full-brief parity. Every base and variant brief is compared one at a time; only query_diagnostics.candidate_rows and query_diagnostics.candidate_index_used are normalized, and their actual before/after values are reported. No installation or production speedup claim.";
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
