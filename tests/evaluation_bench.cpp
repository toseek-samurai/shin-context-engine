#include "context_internal.h"
#include "profile_quality.h"
#include "generated_snapshot.h"

namespace {
using namespace shin::context;
Json summary(const SuiteEvaluation& suite) {
    return {{"total", suite.total}, {"passed", suite.passed}, {"failed", suite.failed},
        {"queries", suite.query_total}, {"queries_passed", suite.query_passed},
        {"failed_case_ids", suite.failed_case_ids}};
}
int benchmark(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: shin-evaluation-bench ROOT OUTPUT PROFILE_DIR PROFILE disk|resident\n";
        return 2;
    }
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]);
    options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]);
    options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    const std::string mode = argv[5];
    if (mode != "disk" && mode != "resident") return 2;
    Json output{{"schema_version", "shin.evaluation-benchmark.v1"}, {"mode", mode},
        {"profile", options.profile.id}, {"gates", Json::array()}};
    std::shared_ptr<const GeneratedSnapshot> snapshot;
    auto start = SteadyClock::now();
    if (mode == "resident") snapshot = GeneratedSnapshot::load(options);
    output["snapshot_load_ms"] = elapsed_ms_since(start);
    for (bool pack : {false, true}) {
        const auto generations = generation_execution_count();
        start = SteadyClock::now();
        // Both independent suites keep their own exact freshness assessment.
        auto session = snapshot ? std::make_unique<RetrievalSession>(snapshot, true) :
                                  std::make_unique<RetrievalSession>(options, true);
        const auto admission = elapsed_ms_since(start);
        start = SteadyClock::now();
        const auto suite = pack ? evaluate_pack_cases(*session) : evaluate_profile_cases(*session);
        output["gates"].push_back({{"command", pack ? "pack-eval" : "eval"},
            {"admission_ms", admission}, {"cases_ms", elapsed_ms_since(start)},
            {"generations", generation_execution_count() - generations}, {"summary", summary(suite)}});
        if (!suite.total || suite.failed) throw std::runtime_error("benchmark quality suite failed");
    }
    output["ok"] = true;
    output["process_peak_rss_bytes"] = process_peak_rss_bytes();
    output["boundary"] = "Two independent strict admissions; typed case execution includes result assertions, but excludes CLI report serialization. Resident load is additional bounded temporary storage, not a freshness proof or deployment change.";
    std::cout << output.dump(2) << '\n';
    return 0;
}
}
int main(int argc, char** argv) {
    try { return benchmark(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
