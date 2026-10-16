#include "context_internal.h"
#include "retrieval_session.h"

#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::context;
    try {
        const bool current = argc == 6 && std::string_view(argv[5]) == "--require-current";
        if (argc != 5 && !current) throw std::runtime_error("Invalid arguments");
        std::string input(65537, '\0');
        std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
        if (std::cin.bad() || !std::cin.eof() || std::cin.gcount() > 65536)
            throw std::runtime_error("Invalid input size");
        input.resize(static_cast<std::size_t>(std::cin.gcount()));
        const Json queries = Json::parse(input);
        if (!queries.is_array() || queries.empty() || queries.size() > 128)
            throw std::runtime_error("Invalid query batch");
        RuntimeOptions options;
        options.root = fs::canonical(argv[1]);
        options.out_root = fs::canonical(argv[2]);
        options.profile_dir = fs::canonical(argv[3]);
        options.profile = make_profile(argv[4], options.profile_dir);
        options.force_index = true;
        RetrievalSession session(options, current ? RetrievalSessionOpenMode::verified_current
                                                 : RetrievalSessionOpenMode::verified_snapshot);
        Json results = Json::array();
        for (const Json& query : queries) {
            if (!query.is_string() || query.get_ref<const std::string&>().empty() ||
                query.get_ref<const std::string&>().size() > 4096)
                throw std::runtime_error("Invalid query");
            auto request = make_command_query_request(options.profile, {query.get<std::string>()}, 20, 2);
            request.require_current = current;
            results.push_back(session.brief(request, BriefProjection::compact));
        }
        std::cout << Json({{"completed", true}, {"snapshot_id", session.freshness().snapshot_id},
                          {"results", std::move(results)}}).dump() << '\n';
        return std::cout ? 0 : 1;
    } catch (const std::exception&) {
        std::cerr << "Lexical snapshot experiment rejected invalid, stale or unavailable input.\n";
        return 1;
    }
}
