#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "profile_quality.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace shin::context {
namespace {

class TempDirectory {
public:
    explicit TempDirectory(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent / ("shin-profile-quality-" + std::to_string(stamp) + "." +
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

class StdoutCapture {
public:
    StdoutCapture() : original_(std::cout.rdbuf(output_.rdbuf())) {}
    ~StdoutCapture() { std::cout.rdbuf(original_); }

    StdoutCapture(const StdoutCapture&) = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    std::string text() const { return output_.str(); }

private:
    std::ostringstream output_;
    std::streambuf* original_;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("profile quality test: " + message);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

template <typename Callback>
void expect_cli_error(Callback&& callback, const std::string& expected_code,
                      const std::string& label) {
    try {
        callback();
    } catch (const CliError& error) {
        expect(error.code() == expected_code,
               label + " returned " + error.code() + " instead of " + expected_code);
        return;
    } catch (const std::exception& error) {
        fail(label + " returned a non-CLI error: " + error.what());
    }
    fail(label + " did not reject the input");
}

template <typename Callback>
SuiteEvaluation evaluate_without_stdout(Callback&& callback, const std::string& label) {
    StdoutCapture capture;
    SuiteEvaluation evaluation = callback();
    expect(capture.text().empty(), label + " wrote to stdout");
    return evaluation;
}

template <typename Callback>
Json run_cli_json(Callback&& callback, int expected_exit_code, const std::string& label) {
    StdoutCapture capture;
    const int exit_code = callback();
    expect(exit_code == expected_exit_code, label + " returned an unexpected exit code");
    const std::string output = capture.text();
    expect(!output.empty(), label + " did not write JSON to stdout");
    return parse_json_text(output, label + " stdout");
}

void write_text(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) fail("cannot create fixture file " + path.string());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) fail("cannot write fixture file " + path.string());
}

Profile fixture_profile() {
    Profile profile;
    profile.id = "profile-quality";
    profile.source_label = "profile quality fixture";
    profile.config_hash = "profile-quality-test-profile-v1";
    profile.root_files = {"README.md"};
    profile.compound_query_first_term_scope = true;
    return profile;
}

Json pack_request_document(const std::string& request_id) {
    return {
        {"schema_version", "shin.task-request.v1"},
        {"request_id", request_id},
        {"evidence_types", {"section", "fact"}},
        {"budget", {{"max_evidence_bytes", 16384}, {"max_records", 16},
                    {"per_query_records", 6}, {"per_document_records", 2}}},
        {"task", {{"objective", "Validate profile quality retrieval."},
                  {"must", {{{"id", "snapshot-evidence"},
                              {"query", "snapshot isolation"}}}}}}
    };
}

EvalCase make_eval_case(std::string id,
                        std::string query,
                        std::vector<std::string> must_paths,
                        std::vector<std::string> query_variants = {}) {
    EvalCase item;
    item.id = std::move(id);
    item.query = std::move(query);
    item.must_paths = std::move(must_paths);
    item.query_variants = std::move(query_variants);
    return item;
}

RuntimeOptions make_fixture_options(const fs::path& base) {
    RuntimeOptions options;
    options.root = base / "root";
    options.out_root = base / "index";
    options.profile = fixture_profile();
    options.force_index = true;

    options.profile.eval_cases = {
        make_eval_case("eval-passing", "snapshot isolation", {"docs/runtime/overview.md"}),
        make_eval_case("eval-failing", "snapshot isolation", {"docs/runtime/missing.md"}),
        make_eval_case("eval-variant-failing", "snapshot isolation",
                       {"docs/runtime/overview.md"}, {"missing revision"})
    };
    PackEvalCase passing_pack;
    passing_pack.id = "pack-passing";
    passing_pack.request_document = pack_request_document("pack-passing-request");
    PackEvalCase failing_pack;
    failing_pack.id = "pack-failing";
    failing_pack.request_document = pack_request_document("pack-failing-request");
    failing_pack.must_paths = {"docs/runtime/missing.md"};
    options.profile.pack_eval_cases = {std::move(passing_pack), std::move(failing_pack)};
    fs::create_directories(options.root);
    return options;
}

void write_generation_sources(const RuntimeOptions& options) {
    write_text(options.root / "README.md",
               "# Profile Quality Fixture\n\n"
               "This fixture validates typed preflight summaries.\n");
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Isolation\n\n"
               "Snapshot isolation preserves immutable retrieval evidence.\n");
    write_text(options.root / "docs/runtime/publisher.md",
               "# Publisher\n\n"
               "The publisher prepares the next snapshot without altering resident evidence.\n");
}

void publish_generation(const RuntimeOptions& options) {
    const GeneratedOutput generated = generate_outputs(options.profile, options.root);
    shin::generated_io::IndexLock writer_lock(
        options.out_root, shin::generated_io::LockMode::exclusive,
        shin::generated_io::LockTarget::writer);
    shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
}

void expect_summary(const SuiteEvaluation& evaluation,
                    std::size_t total,
                    std::size_t passed,
                    std::size_t failed,
                    const std::vector<std::string>& failed_ids,
                    const std::string& label) {
    expect(evaluation.total == total, label + " total mismatch");
    expect(evaluation.passed == passed, label + " passed mismatch");
    expect(evaluation.failed == failed, label + " failed mismatch");
    expect(evaluation.failed_case_ids == failed_ids, label + " failure identifiers mismatch");
}

void expect_same_summary(const SuiteEvaluation& left, const SuiteEvaluation& right,
                         const std::string& label) {
    expect(left.total == right.total && left.passed == right.passed &&
               left.failed == right.failed && left.query_total == right.query_total &&
               left.query_passed == right.query_passed &&
               left.quality_failed_queries == right.quality_failed_queries &&
               left.resource_failed_queries == right.resource_failed_queries &&
               left.failed_case_ids == right.failed_case_ids,
           label + " changed");
}

std::vector<std::string> failed_case_ids_from_json(const Json& rows) {
    std::vector<std::string> ids;
    for (const Json& row : rows) {
        if (row.value("passed", false) || !row.contains("id") || !row.at("id").is_string()) {
            continue;
        }
        const std::string id = row.at("id").get<std::string>();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

void expect_cli_counters(const Json& output, const SuiteEvaluation& evaluation,
                         bool has_query_summary, const std::string& label) {
    const Json& summary = output.at("summary");
    expect(summary.at("total").get<std::size_t>() == evaluation.total,
           label + " total mismatch");
    expect(summary.at("passed").get<std::size_t>() == evaluation.passed,
           label + " passed mismatch");
    expect(summary.at("failed").get<std::size_t>() == evaluation.failed,
           label + " failed mismatch");
    if (has_query_summary) {
        const Json& query_summary = output.at("query_summary");
        expect(query_summary.at("total").get<std::size_t>() == evaluation.query_total,
               label + " query total mismatch");
        expect(query_summary.at("passed").get<std::size_t>() == evaluation.query_passed,
               label + " query passed mismatch");
    } else {
        expect(!output.contains("query_summary"), label + " unexpectedly returned query totals");
    }
}

void expect_cli_failed_ids(const Json& output, const std::string& field,
                           const SuiteEvaluation& evaluation, const std::string& label) {
    expect(failed_case_ids_from_json(output.at(field)) == evaluation.failed_case_ids,
           label + " failure identifiers mismatch");
}

void assert_cli_parity(const RuntimeOptions& options,
                       const SuiteEvaluation& profile_evaluation,
                       const SuiteEvaluation& pack_evaluation) {
    const Json eval = run_cli_json([&] { return run_eval(options, {}); }, 0, "eval");
    expect(!eval.at("strict").get<bool>() && !eval.at("require_current").get<bool>(),
           "non-strict eval flags mismatch");
    expect_cli_counters(eval, profile_evaluation, true, "non-strict eval");
    expect_cli_failed_ids(eval, "cases", profile_evaluation, "non-strict eval");

    const Json strict_eval = run_cli_json(
        [&] { return run_eval(options, {"--strict"}); }, 1, "strict eval");
    expect(strict_eval.at("strict").get<bool>() && !strict_eval.at("require_current").get<bool>(),
           "strict eval flags mismatch");
    expect_cli_counters(strict_eval, profile_evaluation, true, "strict eval");
    expect_cli_failed_ids(strict_eval, "cases", profile_evaluation, "strict eval");

    const Json failed_only_eval = run_cli_json(
        [&] { return run_eval(options, {"--failed-only"}); }, 0, "failed-only eval");
    expect(failed_only_eval.at("projection").get<std::string>() == "failed-only",
           "failed-only eval projection mismatch");
    expect_cli_counters(failed_only_eval, profile_evaluation, true, "failed-only eval");
    expect_cli_failed_ids(failed_only_eval, "failures", profile_evaluation,
                          "failed-only eval");

    const Json pack = run_cli_json([&] { return run_pack_eval(options, {}); }, 0, "pack eval");
    expect(!pack.at("strict").get<bool>() && !pack.at("require_current").get<bool>(),
           "non-strict pack flags mismatch");
    expect_cli_counters(pack, pack_evaluation, false, "non-strict pack eval");
    expect_cli_failed_ids(pack, "cases", pack_evaluation, "non-strict pack eval");

    const Json strict_pack = run_cli_json(
        [&] { return run_pack_eval(options, {"--strict"}); }, 1, "strict pack eval");
    expect(strict_pack.at("strict").get<bool>() &&
               !strict_pack.at("require_current").get<bool>(),
           "strict pack payload flags mismatch");
    expect_cli_counters(strict_pack, pack_evaluation, false, "strict pack eval");
    expect_cli_failed_ids(strict_pack, "cases", pack_evaluation, "strict pack eval");

    const Json failed_only_pack = run_cli_json(
        [&] { return run_pack_eval(options, {"--failed-only"}); }, 0, "failed-only pack eval");
    expect(failed_only_pack.at("projection").get<std::string>() == "failed-only",
           "failed-only pack projection mismatch");
    expect_cli_counters(failed_only_pack, pack_evaluation, false, "failed-only pack eval");
    expect_cli_failed_ids(failed_only_pack, "cases", pack_evaluation,
                          "failed-only pack eval");
}

void assert_concurrent_typed_evaluations(
    const std::shared_ptr<const GeneratedSnapshot>& snapshot,
    const SuiteEvaluation& expected_profile,
    const SuiteEvaluation& expected_pack) {
    RetrievalSession profile_session(snapshot, false);
    RetrievalSession pack_session(snapshot, false);
    SuiteEvaluation profile_evaluation;
    SuiteEvaluation pack_evaluation;
    std::exception_ptr profile_error;
    std::exception_ptr pack_error;
    StdoutCapture capture;
    std::thread profile_thread([&] {
        try {
            profile_evaluation = evaluate_profile_cases(profile_session);
        } catch (...) {
            profile_error = std::current_exception();
        }
    });
    std::thread pack_thread([&] {
        try {
            pack_evaluation = evaluate_pack_cases(pack_session);
        } catch (...) {
            pack_error = std::current_exception();
        }
    });
    profile_thread.join();
    pack_thread.join();
    if (profile_error) std::rethrow_exception(profile_error);
    if (pack_error) std::rethrow_exception(pack_error);
    expect(capture.text().empty(), "concurrent typed evaluation wrote to stdout");
    expect_same_summary(expected_profile, profile_evaluation, "concurrent profile evaluation");
    expect_same_summary(expected_pack, pack_evaluation, "concurrent pack evaluation");
}

void assert_empty_and_malformed_rejection(const RuntimeOptions& options) {
    RuntimeOptions empty_options = options;
    empty_options.profile.eval_cases.clear();
    empty_options.profile.pack_eval_cases.clear();
    RetrievalSession empty_session(empty_options, RetrievalSessionOpenMode::verified_snapshot);
    const SuiteEvaluation empty_eval = evaluate_without_stdout(
        [&] { return evaluate_profile_cases(empty_session); }, "empty profile eval");
    const SuiteEvaluation empty_pack = evaluate_without_stdout(
        [&] { return evaluate_pack_cases(empty_session); }, "empty pack eval");
    expect_summary(empty_eval, 0, 0, 0, {}, "empty profile eval");
    expect(empty_eval.query_total == 0 && empty_eval.query_passed == 0,
           "empty profile eval query summary must be zero");
    expect_summary(empty_pack, 0, 0, 0, {}, "empty pack eval");

    RuntimeOptions malformed_options = options;
    malformed_options.profile.eval_case_config_errors.push_back("invalid-type");
    RetrievalSession malformed_session(malformed_options,
                                       RetrievalSessionOpenMode::verified_snapshot);
    StdoutCapture capture;
    expect_cli_error([&] { (void)evaluate_profile_cases(malformed_session); },
                     "invalid-eval-configuration", "malformed profile eval");
    expect(capture.text().empty(), "malformed profile eval wrote to stdout");

    RuntimeOptions malformed_pack_options = options;
    malformed_pack_options.profile.pack_eval_case_config_errors.push_back("invalid-type");
    RetrievalSession malformed_pack_session(malformed_pack_options,
                                            RetrievalSessionOpenMode::verified_snapshot);
    StdoutCapture malformed_pack_capture;
    expect_cli_error([&] { (void)evaluate_pack_cases(malformed_pack_session); },
                     "invalid-pack-eval-configuration", "malformed pack eval");
    expect(malformed_pack_capture.text().empty(), "malformed pack eval wrote to stdout");

    RuntimeOptions invalid_task_options = options;
    PackEvalCase invalid_task;
    invalid_task.id = "invalid-task";
    invalid_task.request_document = pack_request_document("invalid-task-request");
    invalid_task.request_document.at("task").erase("must");
    invalid_task_options.profile.pack_eval_cases = {std::move(invalid_task)};
    RetrievalSession invalid_task_session(invalid_task_options,
                                          RetrievalSessionOpenMode::verified_snapshot);
    StdoutCapture invalid_task_capture;
    expect_cli_error([&] { (void)evaluate_pack_cases(invalid_task_session); },
                     "task-must-required", "invalid pack task request");
    expect(invalid_task_capture.text().empty(), "invalid pack task request wrote to stdout");
}

int run_profile_quality_test(const fs::path& temporary_parent) {
    TempDirectory temporary(temporary_parent);
    RuntimeOptions options = make_fixture_options(temporary.path());
    write_generation_sources(options);
    publish_generation(options);

    const auto snapshot = GeneratedSnapshot::load(options);
    RetrievalSession session(snapshot, false);
    expect(session.freshness().snapshot_id == snapshot->id(),
           "typed evaluation did not use the supplied snapshot identity");
    expect(session.freshness().status == "not-assessed" && !session.freshness().current,
           "snapshot-only session must not claim current source freshness");

    const SuiteEvaluation profile = evaluate_without_stdout(
        [&] { return evaluate_profile_cases(session); }, "profile evaluation");
    expect_summary(profile, 3, 1, 2, {"eval-failing", "eval-variant-failing"},
                   "profile evaluation");
    expect(profile.query_total == 4 && profile.query_passed == 2,
           "profile query summary mismatch");

    const SuiteEvaluation pack = evaluate_without_stdout(
        [&] { return evaluate_pack_cases(session); }, "pack evaluation");
    expect_summary(pack, 2, 1, 1, {"pack-failing"}, "pack evaluation");
    expect(pack.query_total == 0 && pack.query_passed == 0,
           "pack evaluation must not report query totals");

    assert_cli_parity(options, profile, pack);
    assert_concurrent_typed_evaluations(snapshot, profile, pack);

    const fs::path generated = options.out_root / kGeneratedDir;
    const fs::path hidden = options.out_root / "generated-hidden-for-profile-quality-test";
    fs::rename(generated, hidden);
    try {
        const SuiteEvaluation hidden_profile = evaluate_without_stdout(
            [&] { return evaluate_profile_cases(session); }, "resident profile evaluation");
        const SuiteEvaluation hidden_pack = evaluate_without_stdout(
            [&] { return evaluate_pack_cases(session); }, "resident pack evaluation");
        expect_same_summary(profile, hidden_profile, "resident profile evaluation");
        expect_same_summary(pack, hidden_pack, "resident pack evaluation");
    } catch (...) {
        fs::rename(hidden, generated);
        throw;
    }
    fs::rename(hidden, generated);

    assert_empty_and_malformed_rejection(options);
    RuntimeOptions advisory = options;
    advisory.profile.eval_resource_policy = "advisory";
    for (auto& item : advisory.profile.eval_cases) item.max_candidate_rows = 0;
    RetrievalSession advisory_session(advisory, RetrievalSessionOpenMode::verified_snapshot);
    const auto split = evaluate_without_stdout(
        [&] { return evaluate_profile_cases(advisory_session); }, "advisory typed evaluation");
    expect(split.quality_failed_queries == 2 && split.resource_failed_queries > 0 &&
               split.passed == profile.passed && split.query_passed == profile.query_passed,
           "advisory typed evaluation changed quality admission or lost resource counters");
    const auto resource = eval_resource_check_json(advisory.profile, split);
    expect(resource.at("policy") == "advisory" && resource.at("ok") == false &&
               resource.at("warning_queries") == split.resource_failed_queries,
           "server-facing resource counters lost advisory warnings");
    return 0;
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc > 2) {
            std::cerr << "usage: profile-quality-test [temporary-parent]\n";
            return EXIT_FAILURE;
        }
        const fs::path parent = argc == 2 ? fs::absolute(argv[1]) : fs::temp_directory_path();
        if (!fs::is_directory(parent)) {
            std::cerr << "temporary parent is not a directory: " << parent << '\n';
            return EXIT_FAILURE;
        }
        return shin::context::run_profile_quality_test(parent);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
