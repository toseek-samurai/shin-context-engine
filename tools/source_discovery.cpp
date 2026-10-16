#include "context_internal.h"
#include "source_discovery.h"

#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::context;
    try {
        if (argc < 6 || argc > 8)
            throw std::runtime_error("Usage: shin-source-discovery ROOT INDEX PROFILE_DIR PROFILE QUERIES_JSON [--persist] [--context]");
        bool persist = false, context = false;
        for (int i = 6; i < argc; ++i) {
            const std::string_view flag(argv[i]);
            if (flag == "--persist" && !persist) persist = true;
            else if (flag == "--context" && !context) context = true;
            else throw std::runtime_error("Unsupported source-discovery option");
        }
        RuntimeOptions options;
        options.root = fs::canonical(argv[1]);
        options.out_root = fs::canonical(argv[2]);
        options.profile = make_profile(argv[4], fs::canonical(argv[3]));
        const fs::path request = argv[5];
        if (!fs::is_regular_file(request) || fs::file_size(request) > 256 * 1024)
            throw std::runtime_error("Invalid bounded query file");
        const auto queries = Json::parse(read_file(request)).get<std::vector<std::string>>();
        fs::path cache;
        if (persist) {
            // This one-shot main has no threads or environment mutations.
            const char* home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
#ifdef _WIN32
            if (!home || !*home) home = std::getenv("USERPROFILE"); // NOLINT(concurrency-mt-unsafe)
#endif
            if (!home || !fs::path(home).is_absolute()) throw std::runtime_error("Missing absolute HOME");
            cache = fs::path(home).lexically_normal() / ".shin-source-cache";
        }
        SourceDiscoveryIndex index(options, cache);
        if (context && queries.size() != 1) throw std::runtime_error("Context requires one query");
        std::cout << (context ? index.context(options, queries.front()) : index.query(options, queries)).dump() << '\n';
        return 0;
    } catch (const CliError& error) {
        std::cout << Json({{"schema_version", "shin.source-candidates.v1"}, {"ok", false}, {"error", error.code()}}).dump() << '\n';
    } catch (const std::exception&) {
        std::cout << "{\"schema_version\":\"shin.source-candidates.v1\",\"ok\":false,\"error\":\"invalid-source-discovery\"}\n";
    }
    return 1;
}
