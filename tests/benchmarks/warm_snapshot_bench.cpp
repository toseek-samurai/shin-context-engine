#include "context_internal.h"
#include "generated_snapshot.h"
#include "retrieval_session.h"
#include <charconv>

namespace {
using namespace shin::context;
using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json distribution(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double fraction) {
        return samples[static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size()))) - 1];
    };
    return {{"p50_ms", percentile(0.5)}, {"p95_ms", percentile(0.95)},
            {"min_ms", samples.front()}, {"max_ms", samples.back()}};
}

int run_benchmark(int argc, char** argv) {
    if (argc < 6 || argc > 8 || (argc >= 7 && std::string_view(argv[6]) != "--snapshot-only")) {
        std::cerr << "usage: shin-warm-snapshot-bench ROOT OUTPUT PROFILE_DIR PROFILE QUERY [--snapshot-only [MAX_FILE_BYTES]]\n";
        return 2;
    }
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]);
    options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]);
    options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    SnapshotLoadLimits limits;
    if (argc == 8) {
        const std::string_view value(argv[7]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limits.maximum_file_bytes);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            limits.maximum_file_bytes == 0 || limits.maximum_file_bytes > limits.maximum_payload_bytes)
            throw std::runtime_error("invalid explicit route file budget");
    }
    const QueryRequest request = make_command_query_request(options.profile, {argv[5]}, 6, 2);
    const auto load_started = Clock::now();
    auto snapshot = GeneratedSnapshot::load(options, limits);
    const auto load_ms = elapsed_ms(load_started);
    Json expected;
    {
        RetrievalSession disk(options, RetrievalSessionOpenMode::verified_snapshot);
        expected = disk.brief(request, BriefProjection::compact);
    }
    if (expected.at("evidence").empty()) throw std::runtime_error("benchmark query must retrieve evidence");
    const auto expected_bytes = expected.dump();
    std::vector<double> disk_samples;
    std::vector<double> memory_samples;
    constexpr int iterations = 40;
    for (int iteration = -2; iteration < iterations; ++iteration) {
        auto disk = [&] {
            const auto start = Clock::now();
            RetrievalSession session(options, RetrievalSessionOpenMode::verified_snapshot);
            const auto payload = session.brief(request, BriefProjection::compact).dump();
            const auto duration = elapsed_ms(start);
            if (payload != expected_bytes) throw std::runtime_error("disk generation or evidence changed during benchmark");
            if (iteration >= 0) disk_samples.push_back(duration);
        };
        auto memory = [&] {
            const auto start = Clock::now();
            RetrievalSession session(snapshot, false);
            const auto payload = session.brief(request, BriefProjection::compact).dump();
            const auto duration = elapsed_ms(start);
            if (payload != expected_bytes) throw std::runtime_error("memory evidence differs from disk baseline");
            if (iteration >= 0) memory_samples.push_back(duration);
        };
        if (iteration % 2 == 0) { disk(); memory(); }
        else { memory(); disk(); }
    }
    // Currentness admission is measured separately; it is never included in the snapshot-only speedup.
    auto current_request = request;
    current_request.require_current = true;
    std::vector<double> current_disk;
    std::vector<double> current_memory;
    const int audit_iterations = argc >= 7 ? 0 : 10;
    for (int iteration = 0; iteration < audit_iterations; ++iteration) {
        const auto disk_start = Clock::now();
        Json disk;
        {
            RetrievalSession session(options, true);
            disk = session.brief(current_request, BriefProjection::compact);
        }
        current_disk.push_back(elapsed_ms(disk_start));
        const auto memory_start = Clock::now();
        RetrievalSession session(snapshot, true);
        const auto memory = session.brief(current_request, BriefProjection::compact);
        current_memory.push_back(elapsed_ms(memory_start));
        if (disk != memory) throw std::runtime_error("current disk/memory admission or evidence differ");
    }
    const auto disk = distribution(disk_samples);
    const auto memory = distribution(memory_samples);
    std::cout << Json{
        {"schema_version", "shin.warm-snapshot-benchmark.v1"}, {"ok", true},
        {"version", kProductVersion}, {"snapshot_id", snapshot->id()}, {"profile", options.profile.id},
        {"query", request.query}, {"iterations", iterations}, {"verified_load_ms", load_ms},
        {"resident_payload_bytes", snapshot->payload_bytes()}, {"response_bytes", expected_bytes.size()},
        {"snapshot_only", {{"disk", disk}, {"resident", memory},
                            {"p50_ratio", disk.at("p50_ms").get<double>() / memory.at("p50_ms").get<double>()}}},
        {"exact_current", {{"iterations", audit_iterations},
            {"disk", audit_iterations ? distribution(current_disk) : Json(nullptr)},
            {"resident", audit_iterations ? distribution(current_memory) : Json(nullptr)}}},
        {"process_peak_rss_bytes", process_peak_rss_bytes()},
        {"boundary", "Paired single-process native sessions including admission, Brief and serialization. Disk inputs may be OS-cache warm. Snapshot-only compares verified_snapshot disk opens against admitted immutable bytes, without source freshness; it is not the normal CLI default. Exact-current uses the normal CLI require-current constructor and repeats admission for both paths. Peak RSS includes both modes and temporary allocations. No HTTP or multi-client server claim."}
    }.dump(2) << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return run_benchmark(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
