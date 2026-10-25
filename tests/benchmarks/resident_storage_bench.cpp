#include "context_internal.h"
#include "generated_snapshot.h"
#include "retrieval_session.h"
#include "source_discovery.h"

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace {
using namespace shin::context;
Json memory() {
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return {{"rss_bytes", info.resident_size}, {"footprint_bytes", info.phys_footprint}};
#endif
    return {{"rss_bytes", nullptr}, {"footprint_bytes", nullptr}};
}
}

int main(int argc, char** argv) try {
    using namespace shin::context;
    if (argc != 7) throw std::runtime_error("Expected ROOT INDEX PROFILE_DIR PROFILE CACHE QUERY");
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]);
    options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]);
    options.profile = make_profile(argv[4], options.profile_dir);
    const auto started = SteadyClock::now();
    // This test measures storage, not readiness or a new qualification receipt.
    // The larger Retool route already has owner-approved384/192MiB load caps.
    auto snapshot = GeneratedSnapshot::load(options, {384ULL * 1024 * 1024, 192ULL * 1024 * 1024});
    options.generated_snapshot = snapshot;
    SourceDiscoveryIndex source(options, fs::canonical(argv[5]));
    const double preparation = elapsed_ms_since(started);
    const auto prepared_memory = memory();
    const auto request = make_command_query_request(options.profile, {"--", argv[6]}, 4, 2);
    std::vector<double> samples;
    std::string expected;
    for (int iteration = -2; iteration < 20; ++iteration) {
        const auto began = SteadyClock::now();
        RetrievalSession session(snapshot, false);
        const auto documents = session.brief(request, BriefProjection::compact);
        const auto candidates = source.context_at_snapshot(options, argv[6]);
        Json rows = candidates.at("results");
        for (auto& row : rows) row.erase("query_ms");
        const auto evidence = Json{{"documents", documents}, {"candidates", rows},
                                   {"context", candidates.at("context")}}.dump();
        if (iteration == -2) expected = evidence;
        else if (evidence != expected) throw std::runtime_error("Evidence changed during the paired storage probe");
        if (iteration >= 0) samples.push_back(elapsed_ms_since(began));
    }
    std::sort(samples.begin(), samples.end());
    std::cout << Json{{"ok", true}, {"root", options.root.string()}, {"snapshot", snapshot->id()},
        {"evidence_hash", fnv1a64(expected)}, {"payload_bytes", snapshot->payload_bytes()},
        {"preparation_ms", preparation}, {"prepared_memory", prepared_memory}, {"steady_memory", memory()},
        {"process_peak_rss_bytes", process_peak_rss_bytes()}, {"iterations", 20},
        {"query_p50_ms", samples[9]}, {"query_p95_ms", samples[18]},
        {"boundary", "Single retained snapshot/source-index; no Observer readiness, model, publication or multi-project capacity claim."}}.dump(2) << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
