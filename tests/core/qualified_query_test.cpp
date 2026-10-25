#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "retrieval_session.h"

#include <chrono>
#include <iostream>

namespace shin::context {
namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("qualified query: " + message);
}

void check_qualified_token_boundaries() {
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"DeltaService::apply", {"delta-service", "apply"}},
        {"`::engine::DeltaService::apply()`", {"engine", "delta-service", "apply"}},
        {"find ns::is", {"find", "ns", "is"}},
        {"the ns::is and ns::as", {"ns", "is", "as"}},
        {"ns::is ns::is", {"ns", "is"}},
        {"ns::_member2", {"ns", "_member2"}},
        {"DeltaService:apply", {"delta-service:apply"}},
        {"DeltaService:::apply", {"delta-service:::apply"}},
        {"https://example.org/a", {"https://example.org/a"}},
        {"src/DeltaService.cpp", {"src/delta-service.cpp"}},
        {"thing::vector<int>", {"thing::vector<int"}},
        {"thing::vector<>", {"thing::vector"}},
        {"thing::vector<>()", {"thing::vector"}},
    };
    for (const auto& [query, terms] : cases) {
        expect(parse_terms(query) == terms, "token boundary: " + query);
    }
}

class Fixture {
public:
    explicit Fixture(const fs::path& parent)
        : base_(parent / ("qualified-query-" + std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directory(base_);
        options.root = base_ / "root";
        options.out_root = base_ / "index";
        options.force_index = true;
        options.profile.id = "qualified-query";
        options.profile.config_hash = "qualified-query-fixture-v1";
        options.profile.root_files = {"README.md"};
        options.profile.source_fact_roots = {"src"};
        options.profile.source_fact_extensions = {".cpp"};
        options.profile.source_fact_extractors = {"language"};
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(base_, error);
    }
    void write(const fs::path& path, std::string_view text) const {
        fs::create_directories((options.root / path).parent_path());
        std::ofstream stream(options.root / path);
        stream << text;
        stream.close();
        expect(!stream.fail(), "fixture write failed");
    }
    RuntimeOptions options;
private:
    fs::path base_;
};

void check_qualified_retrieval_parity(const fs::path& parent) {
    Fixture fixture(parent);
    fixture.write("README.md", "# Neutral lexical lookup fixture\n");
    fixture.write("src/target.cpp", "int DeltaService::apply() { return 7; }\n");
    fixture.write("src/owner_only.cpp", "int DeltaService::reset() { return 8; }\n");
    fixture.write("src/member_only.cpp", "int OtherService::apply() { return 9; }\n");
    {
        const auto output = generate_outputs(fixture.options.profile, fixture.options.root);
        shin::generated_io::IndexLock writer(fixture.options.out_root,
            shin::generated_io::LockMode::exclusive, shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(fixture.options.out_root, kGeneratedDir, output.files);
    }
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession disk(fixture.options, true);
    RetrievalSession resident(snapshot, true);
    for (const std::string query : {"DeltaService::apply", "::DeltaService::apply()"}) {
        const auto request = make_command_query_request(fixture.options.profile, {query}, 6, 2);
        expect(request.term_groups.size() == 2, "qualifiers remain distinct groups");
        const Json search = disk.search(request);
        const Json context = disk.context(request);
        expect(search == resident.search(request), "disk/resident search parity");
        expect(context == resident.context(request), "disk/resident context parity");
        for (const Json* result : {&search, &context}) {
            const char* field = result == &search ? "hits" : "facts";
            const auto& hits = result->at(field);
            expect(hits.size() == 1 && hits.at(0).at("path") == "src/target.cpp",
                   std::string(field) + " target only, independent of parity");
            expect(hits.at(0).at("matched_group_indices") == Json::array({0, 1}),
                   std::string(field) + " exact qualifier/member coverage");
            expect(result->at("retrieval_mode") == "strict", "search/context should not relax");
        }
        expect(context.at("sections").empty(), "no unrelated section evidence");
        const Json brief = disk.brief(request, BriefProjection::compact);
        expect(brief == resident.brief(request, BriefProjection::compact), "disk/resident brief parity");
        expect(brief.at("retrieval_mode") == "strict", "qualified query should not relax");
        expect(brief.at("query_coverage").at("covered_groups") == 2, "complete lexical coverage");
        const auto& paths = brief.at("ranked_paths");
        expect(paths.size() == 1 && paths.at(0).at("path") == "src/target.cpp",
               "owner-only and member-only distractors must not pass");
        expect(brief.at("verification_paths").at(0).at("ranges").at(0).at("start_line") == 1,
               "verification range survives qualified lookup");
    }
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected fixture parent");
        shin::context::check_qualified_token_boundaries();
        shin::context::check_qualified_retrieval_parity(argv[1]);
        std::cout << "qualified query: 2 checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
