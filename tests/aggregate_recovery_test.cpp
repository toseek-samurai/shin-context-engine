#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "profile_quality.h"
#include "retrieval_session.h"
#include "aggregate_recovery_experiment.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace shin::context;
using namespace shin::context::experiment;

constexpr const char* kStrictQuery = "common alpha bravo charlie delta echo foxtrot needle";

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("aggregate recovery: " + message);
}

class TempDirectory {
public:
    explicit TempDirectory(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent / ("shin-aggregate-recovery-" + std::to_string(stamp) + "." +
                          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        fs::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const fs::path& path() const noexcept { return path_; }
private:
    fs::path path_;
};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create fixture file " + path.string());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("cannot write fixture file " + path.string());
}

RuntimeOptions make_options(const fs::path& base) {
    RuntimeOptions options;
    options.root = base / "root";
    options.out_root = base / "index";
    options.force_index = true;
    options.profile.id = "aggregate-recovery-fixture";
    options.profile.config_hash = "aggregate-recovery-fixture-v1";
    options.profile.root_files = {"README.md"};
    EvalCase resource_case;
    resource_case.id = "strict-recovered-row-count";
    resource_case.query = kStrictQuery;
    resource_case.must_paths = {"docs/segment-0.md"};
    resource_case.max_candidate_rows = 100;
    options.profile.eval_cases.push_back(resource_case);
    fs::create_directories(options.root);
    write_text(options.root / "README.md", "# Aggregate recovery fixture\n");
    return options;
}

void write_generated_sources(const RuntimeOptions& options) {
    constexpr int kFiles = 54;
    constexpr int kSectionsPerFile = 400;
    constexpr std::string_view common =
        "common alpha bravo charlie delta echo foxtrot golf";
    for (int file = 0; file < kFiles; ++file) {
        std::ostringstream source;
        for (int section = 0; section < kSectionsPerFile; ++section) {
            source << "# " << common;
            if (file < 4) source << " needle";
            if (file < 2 && section < 10) {
                source << " rareone raretwo rarethree rarefour rarefive raresix";
            }
            if (file < 27) {
                source << " unionalpha unionbravo unioncharlie uniondelta";
            } else {
                source << " unionecho unionfoxtrot uniongolf unionhotel";
            }
            source << " row" << file << "x" << section << "\n\n";
            source << "Generated fixture section.\n\n";
        }
        write_text(options.root / "docs" / ("segment-" + std::to_string(file) + ".md"), source.str());
    }

    // A legal prefix-completion overflow: two bounded source files contribute
    // more than the native 2,048-term completion budget for the "pref" prefix.
    for (int file = 0; file < 2; ++file) {
        std::ostringstream prefix_source;
        prefix_source << "# common alpha bravo charlie delta echo foxtrot\n\n";
        for (int term = file * 1025; term < (file + 1) * 1025; ++term) {
            prefix_source << "prefx" << term << ' ';
        }
        prefix_source << '\n';
        write_text(options.root / "docs" / ("prefix-budget-" + std::to_string(file) + ".md"),
                   prefix_source.str());
    }
}

void publish_native_index(const RuntimeOptions& options) {
    const GeneratedOutput generated = generate_outputs(options.profile, options.root);
    shin::generated_io::IndexLock writer(
        options.out_root, shin::generated_io::LockMode::exclusive,
        shin::generated_io::LockTarget::writer);
    shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
}

QueryRequest request_for(const Profile& profile, const std::string& query) {
    return make_command_query_request(profile, {query}, 2, 1);
}

Json run_brief(RetrievalSession& session, const Profile& profile,
               const std::string& query, bool candidate) {
    configure_aggregate_recovery(candidate);
    QueryRequest request = request_for(profile, query);
    return session.brief(request, BriefProjection::full);
}

Json run_prefix_brief(RetrievalSession& session, const Profile& profile, bool candidate) {
    constexpr const char* query = "pref common alpha bravo charlie delta echo foxtrot";
    configure_aggregate_recovery(candidate);
    QueryRequest request = request_for(profile, query);
    expect(request.prepared_term_groups.size() == 8 &&
               !request.prepared_term_groups.front().empty(),
           "prefix fixture query did not prepare eight groups");
    request.prepared_term_groups.front().front().allow_prefix = true;
    return session.brief(request, BriefProjection::full);
}

Json normalized_brief(Json value) {
    auto& diagnostics = value.at("query_diagnostics");
    diagnostics.erase("candidate_rows");
    diagnostics.erase("candidate_index_used");
    return value;
}

Json run_profile_eval(RetrievalSession& session, bool candidate) {
    configure_aggregate_recovery(candidate);
    const SuiteEvaluation suite = evaluate_profile_cases(session);
    return {{"total", suite.total}, {"passed", suite.passed}, {"failed", suite.failed},
            {"query_total", suite.query_total}, {"query_passed", suite.query_passed},
            {"quality_failed_queries", suite.quality_failed_queries},
            {"resource_failed_queries", suite.resource_failed_queries}};
}

void check_recovery_and_fallback(const fs::path& parent) {
    TempDirectory temporary(parent);
    RuntimeOptions options = make_options(temporary.path());
    write_generated_sources(options);
    publish_native_index(options);
    RetrievalSession session(options, false);

    const Json baseline_strict = run_brief(session, options.profile, kStrictQuery, false);
    const Json baseline_metrics = aggregate_metrics();
    expect(baseline_metrics.at("attempts") == 0, "disabled recovery attempted an aggregate retry");
    const Json recovered_strict = run_brief(session, options.profile, kStrictQuery, true);
    const Json strict_metrics = aggregate_metrics();
    expect(normalized_brief(recovered_strict) == normalized_brief(baseline_strict),
           "strict recovery changed ranking, fields, provenance, or other brief output");
    expect(recovered_strict.at("query_diagnostics").at("candidate_index_used") == true,
           "strict selective intersection was not recovered through the terms index");
    expect(recovered_strict.at("query_diagnostics").at("candidate_rows").get<std::size_t>() > 0 &&
               recovered_strict.at("query_diagnostics").at("candidate_rows").get<std::size_t>() <=
                   kMaxIndexedCandidateRows,
           "strict recovery did not report the actual bounded intersection size");
    expect(strict_metrics.at("attempts") == 1 && strict_metrics.at("recovered") == 1 &&
               strict_metrics.at("still_scan") == 0,
           "strict recovery metrics mismatch");
    expect(strict_metrics.at("retries").at(0).at("required") == 8 &&
               strict_metrics.at("retries").at(0).at("before_indexed") == false &&
               strict_metrics.at("retries").at(0).at("after_indexed") == true,
           "strict retry did not show an unusable prepared set becoming indexed");

    // Resource assertions consume recovered candidate counts. Four matching
    // documents each contribute 400 rows, so this deliberately small ceiling
    // must fail on the true indexed count even though a scan fallback reports 0.
    const Json baseline_eval = run_profile_eval(session, false);
    const Json candidate_eval = run_profile_eval(session, true);
    expect(baseline_eval.at("resource_failed_queries") == 0,
           "baseline scan unexpectedly failed the candidate-row resource assertion");
    expect(candidate_eval.at("resource_failed_queries") == 1,
           "recovered candidate count did not drive the resource assertion");

    constexpr const char* relaxed_query =
        "common alpha bravo charlie delta echo foxtrot missingtoken";
    const Json baseline_relaxed = run_brief(session, options.profile, relaxed_query, false);
    const Json recovered_relaxed = run_brief(session, options.profile, relaxed_query, true);
    const Json relaxed_metrics = aggregate_metrics();
    expect(normalized_brief(recovered_relaxed) == normalized_brief(baseline_relaxed),
           "over-cap relaxed recovery changed ranking, fields, provenance, or other brief output");
    expect(recovered_relaxed.at("query_diagnostics").at("candidate_index_used") == false,
           "over-cap relaxed candidate set was marked indexed");
    expect(recovered_relaxed.at("query_diagnostics").at("candidate_rows").get<std::size_t>() >
               kMaxIndexedCandidateRows,
           "over-cap relaxed fallback did not report the full discovered row count");
    expect(relaxed_metrics.at("attempts") == 2 && relaxed_metrics.at("recovered") == 1 &&
               relaxed_metrics.at("still_scan") == 1 &&
               relaxed_metrics.at("retries").at(0).at("required") == 8 &&
               relaxed_metrics.at("retries").at(0).at("after_rows") == 0 &&
               relaxed_metrics.at("retries").at(0).at("after_indexed") == true &&
               relaxed_metrics.at("retries").at(1).at("required") == 4 &&
               relaxed_metrics.at("retries").at(1).at("after_indexed") == false,
           "relaxed over-cap retry did not retain the full-scan fallback");

    constexpr const char* relaxed_selective_query =
        "common rareone raretwo rarethree rarefour rarefive raresix absenttoken";
    const Json baseline_relaxed_selective =
        run_brief(session, options.profile, relaxed_selective_query, false);
    const Json recovered_relaxed_selective =
        run_brief(session, options.profile, relaxed_selective_query, true);
    const Json relaxed_selective_metrics = aggregate_metrics();
    expect(normalized_brief(recovered_relaxed_selective) == normalized_brief(baseline_relaxed_selective),
           "selective relaxed recovery changed ranking, fields, provenance, or other brief output");
    expect(recovered_relaxed_selective.at("query_diagnostics").at("required_group_matches") == 4 &&
               recovered_relaxed_selective.at("query_diagnostics").at("candidate_index_used") == true &&
               recovered_relaxed_selective.at("query_diagnostics").at("candidate_rows").get<std::size_t>() > 0 &&
               recovered_relaxed_selective.at("query_diagnostics").at("candidate_rows").get<std::size_t>() <=
                   kMaxIndexedCandidateRows,
           "selective relaxed intersection was not recovered as a bounded indexed result");
    expect(relaxed_selective_metrics.at("attempts") == 2 &&
               relaxed_selective_metrics.at("retries").at(0).at("required") == 8 &&
               relaxed_selective_metrics.at("retries").at(0).at("after_rows") == 0 &&
               relaxed_selective_metrics.at("retries").at(1).at("required") == 4 &&
               relaxed_selective_metrics.at("retries").at(1).at("after_indexed") == true &&
               relaxed_selective_metrics.at("retries").at(1).at("after_rows") <= kMaxIndexedCandidateRows,
           "selective relaxed retry metrics mismatch");

    constexpr const char* empty_query =
        "common alpha bravo missingone missingtwo missingthree missingfour missingfive";
    const Json baseline_empty = run_brief(session, options.profile, empty_query, false);
    const Json recovered_empty = run_brief(session, options.profile, empty_query, true);
    const Json empty_metrics = aggregate_metrics();
    expect(normalized_brief(recovered_empty) == normalized_brief(baseline_empty),
           "empty intersection changed the full-scan baseline result");
    expect(empty_metrics.at("attempts") == 2 && empty_metrics.at("recovered") == 2 &&
               empty_metrics.at("retries").at(1).at("required") == 4 &&
               empty_metrics.at("retries").at(1).at("after_rows") == 0 &&
               empty_metrics.at("retries").at(1).at("after_indexed") == true &&
               empty_metrics.at("retries").at(1).at("coverage_complete") == true,
           "empty intersection recovery metrics mismatch");

    const Json baseline_prefix = run_prefix_brief(session, options.profile, false);
    const Json candidate_prefix = run_prefix_brief(session, options.profile, true);
    const Json prefix_metrics = aggregate_metrics();
    expect(candidate_prefix == baseline_prefix,
           "incomplete prefix coverage changed ranking, fields, provenance, or scan result");
    expect(candidate_prefix.at("query_diagnostics").at("candidate_index_used") == false,
           "incomplete prefix completion was trusted as indexed");
    expect(prefix_metrics.at("attempts") == 0 && prefix_metrics.at("retries").empty(),
           "incomplete term-index coverage triggered aggregate recovery");

    constexpr const char* union_overflow_query =
        "unionalpha unionbravo unioncharlie uniondelta unionecho unionfoxtrot uniongolf unionhotel";
    const Json baseline_union = run_brief(session, options.profile, union_overflow_query, false);
    const Json candidate_union = run_brief(session, options.profile, union_overflow_query, true);
    const Json union_metrics = aggregate_metrics();
    expect(candidate_union == baseline_union,
           "individually bounded groups with an over-cap union changed the baseline result");
    expect(candidate_union.at("query_diagnostics").at("required_group_matches") == 4 &&
               candidate_union.at("query_diagnostics").at("candidate_index_used") == false &&
               candidate_union.at("query_diagnostics").at("candidate_rows").get<std::size_t>() >
                   kMaxIndexedCandidateRows,
           "over-cap relaxed union did not preserve the bounded full-scan fallback");
    expect(union_metrics.at("attempts") == 0 && union_metrics.at("retries").empty(),
           "aggregate recovery retried a final union overflow when every group was individually indexed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected a temporary parent directory");
        check_recovery_and_fallback(argv[1]);
        std::cout << "aggregate recovery: checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
