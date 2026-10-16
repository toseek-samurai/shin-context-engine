#include "source_discovery.h"
#include "context_internal.h"

#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::context;
    try {
        if (argc != 6) throw std::runtime_error("Expected ROOT INDEX PROFILE_DIR PROFILE QUERIES_JSON");
        RuntimeOptions options;
        options.root = fs::canonical(argv[1]);
        options.out_root = fs::canonical(argv[2]);
        options.profile = make_profile(argv[4], fs::canonical(argv[3]));
        if (!fs::is_regular_file(argv[5]) || fs::file_size(argv[5]) > 256 * 1024)
            throw std::runtime_error("Invalid bounded query file");
        const auto queries = Json::parse(read_file(argv[5])).get<std::vector<std::string>>();
        const auto control = discover_source_candidates(options, queries);
        SourceDiscoveryIndex resident(options);
        Json batches = Json::array();
        bool parity = true;
        for (int round = 0; round < 4; ++round) {
            auto result = resident.query(options, queries);
            parity = parity && result.at("snapshot_id") == control.at("snapshot_id") &&
                result.at("reuse").at("query_sequence") == round + 1 &&
                result.at("reuse").at("index_builds") == 1 &&
                result.at("results").size() == control.at("results").size();
            for (std::size_t i = 0; i < queries.size(); ++i)
                parity = parity && result.at("results")[i].at("candidates") == control.at("results")[i].at("candidates");
            batches.push_back(std::move(result));
        }
        std::cout << Json({{"schema_version", "shin.source-discovery-reuse-bench.v1"}, {"ok", parity},
            {"model_runs", 0}, {"index_instances", 2}, {"source_mutated", false},
            {"control", control}, {"resident_batches", std::move(batches)}}).dump() << '\n';
        return parity ? 0 : 1;
    } catch (const std::exception&) {
        std::cout << "{\"schema_version\":\"shin.source-discovery-reuse-bench.v1\",\"ok\":false}\n";
        return 1;
    }
}
