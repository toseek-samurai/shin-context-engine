#include "context_internal.h"
#include "profile_quality.h"

// Independent reference executable: linked to ordinary Core only.
namespace {
using namespace shin::context;
Json summary(const SuiteEvaluation& suite) {
    return {{"total", suite.total}, {"passed", suite.passed}, {"failed", suite.failed},
        {"queries", suite.query_total}, {"queries_passed", suite.query_passed},
        {"quality_failed_queries", suite.quality_failed_queries},
        {"resource_failed_queries", suite.resource_failed_queries},
        {"failed_case_ids", suite.failed_case_ids}};
}
int write_oracle(int argc, char** argv) {
    if (argc != 5) throw std::runtime_error("usage: shin-eval-batch-oracle ROOT OUT PROFILE_DIR PROFILE");
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]); options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]); options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    RetrievalSession session(options, true);
    const auto eval = evaluate_profile_cases(session); // Includes native suite bounds validation.
    for (const auto& item : options.profile.eval_cases) {
        auto emit = [&](const std::string& query) {
            QueryRequest request;
            request.query = query; request.terms = parse_terms(query);
            request.limit = item.limit > 0 ? item.limit : 8;
            request.per_doc_limit = item.per_doc_limit > 0 ? item.per_doc_limit : 2;
            apply_profile_query_expansion(options.profile, request);
            std::cout << session.brief(request).dump() << '\n';
        };
        emit(item.query);
        for (const auto& query : item.query_variants) emit(query);
    }
    std::cout << Json({{"eval", summary(eval)}, {"pack", summary(evaluate_pack_cases(session))}}).dump() << '\n';
    return 0;
}
}
int main(int argc, char** argv) {
    try { return write_oracle(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
