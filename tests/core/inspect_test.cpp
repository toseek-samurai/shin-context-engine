#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"

#include <chrono>

namespace shin::context {
namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("inspect: " + message);
}

template <typename Callback>
void rejects(Callback callback, const std::string& code) {
    try { callback(); }
    catch (const CliError& error) {
        expect(error.code() == code, "expected " + code + ", got " + error.code());
        return;
    }
    throw std::runtime_error("inspect: expected rejection " + code);
}

class Fixture {
public:
    explicit Fixture(const fs::path& parent) : base(parent / ("inspect-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()))) {
        options.root = base / "source";
        options.out_root = base / "index";
        options.profile.id = "inspect-fixture";
        options.profile.config_hash = "inspect-fixture-v1";
        options.profile.root_files = {"README.md"};
        options.profile.source_fact_roots = {"src"};
        options.profile.source_fact_extensions = {".cpp"};
        options.profile.source_fact_extractors = {"language"};
        write("README.md", "# Fixture\n\n## Outline\n\nPublished document.\n");
    }
    ~Fixture() { std::error_code error; fs::remove_all(base, error); }
    void write(const fs::path& path, const std::string& content) {
        fs::create_directories((options.root / path).parent_path());
        std::ofstream stream(options.root / path);
        stream << content;
        stream.close();
        expect(!stream.fail(), "write fixture");
    }
    void publish() {
        const auto generated = generate_outputs(options.profile, options.root);
        shin::generated_io::IndexLock lock(options.out_root, shin::generated_io::LockMode::exclusive,
                                           shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
    }
    fs::path base;
    RuntimeOptions options;
};

void inspect_checks(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string path = "src/pipeline space.cpp";
    fixture.write(path, "int acquire_pending() {\n return 1;\n}\n\n"
                        "int confirm_delivery() {\n return 2;\n}\n\n"
                        "int record_failure() {\n return 3;\n}\n");
    fixture.write("src/pipeline.cpp", "int unrelated() { return 4; }\n");
    fixture.publish();
    auto snapshot = GeneratedSnapshot::load(fixture.options);
    Json outline;
    {
        RetrievalSession disk(fixture.options, true), resident(snapshot, true);
        outline = disk.inspect(path);
        expect(outline == resident.inspect(path), "cold/warm exact parity");
        expect(outline.at("coverage").at("complete") == true, "complete small inventory");
        std::set<std::string> symbols;
        int previous = 0;
        for (const auto& row : outline.at("records")) {
            expect(row.at("path") == path, "literal path, no neighbor records");
            expect(row.at("start_line").get<int>() >= previous, "physical order");
            previous = row.at("start_line");
            expect(!row.contains("summary") && !row.contains("score") && !row.contains("indexed_text"), "metadata only");
            if (row.contains("symbol_name")) {
                symbols.insert(row.at("symbol_name"));
                expect(row.contains("range_origin") && row.contains("span_hash"), "source provenance");
            }
        }
        expect(symbols == std::set<std::string>{"acquire_pending", "confirm_delivery", "record_failure"}, "discover declarations");
        const auto syntax = disk.inspect("src/pipeline.cpp");
        expect(syntax == resident.inspect("src/pipeline.cpp"), "one-line cold/warm parity");
        expect(syntax.at("records").size() == 1 &&
               syntax.at("records").at(0).at("range_origin") == "syntax" &&
               syntax.at("records").at(0).at("record_type") == "cpp-function" &&
               syntax.at("records").at(0).at("symbol_name") == "unrelated",
               "one-line function is an observed syntax declaration");
        expect(!disk.inspect("README.md").at("records").empty(), "document sections supported");
        const auto absent = disk.inspect("src/not-enrolled.cpp");
        expect(absent.at("records").empty() && absent.at("coverage").at("complete") == true, "empty means no indexed records");
        for (const std::string bad : {"", "/tmp/x", "../x", "src/../x", "./README.md", "src//x", "src/x\n"})
            rejects([&] { (void)disk.inspect(bad); }, "invalid-inspect-path");
    }
    fixture.write(path, "int changed() { return 0; }\n");
    rejects([&] { (void)RetrievalSession(fixture.options, true); }, "stale-index");
    RetrievalSession resident(snapshot, false);
    const auto old = resident.inspect(path);
    expect(old.at("records") == outline.at("records"), "immutable resident inventory after source changes");
    expect(old.at("index_freshness").at("current") == false, "no fabricated currentness");
    {
        std::ofstream corrupt(fixture.options.out_root / kGeneratedDir / "facts.jsonl", std::ios::app);
        corrupt << "corrupt\n";
    }
    bool rejected = false;
    try { (void)RetrievalSession(fixture.options, RetrievalSessionOpenMode::verified_snapshot); }
    catch (const std::exception&) { rejected = true; }
    expect(rejected, "corrupt disk publication rejected");
    expect(resident.inspect(path) == old, "resident bytes independent of publication corruption");
}

void inspect_limits(const fs::path& parent) {
    Fixture fixture(parent);
    std::string content;
    for (int i = 0; i < 150; ++i) content += "int operation_" + std::to_string(i) + "() {\n return 0;\n}\n";
    fixture.write("src/many.cpp", content);
    std::string document;
    for (int i = 0; i < 50; ++i) document += "## " + std::to_string(i) + std::string(2000, 'a') + "\n\nBody.\n\n";
    fixture.write("README.md", document);
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession disk(fixture.options, true), resident(snapshot, true);
    for (const std::string path : {"src/many.cpp", "README.md"}) {
        const auto result = disk.inspect(path);
        expect(result == resident.inspect(path), "bounded cold/warm parity");
        expect(result.at("coverage").at("complete") == false, "explicit truncation");
        expect(result.at("records").size() <= 64 && result.dump(2).size() + 1 <= 65536, "hard output limits");
        if (path == "src/many.cpp") expect(result.at("coverage").at("unprobed_records").is_null(), "unknown tail not guessed");
        else expect(result.at("coverage").at("omitted_probed_records").get<int>() > 0, "whole record byte omission");
    }
}

void inspect_relations_checks(const fs::path& parent) {
    Fixture fixture(parent);
    fixture.options.profile.source_fact_extensions = {".ts"};
    fixture.write("src/owner.ts", "import { dependency } from './dependency';\nimport './missing';\nexport const owner = dependency;\n");
    fixture.write("src/dependency.ts", "export const dependency = 2;\n");
    fixture.write("README.md", "# Fixture\n\n[Owner](src/owner.ts)\n");
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession disk(fixture.options, true), resident(snapshot, true);
    const auto owner = disk.inspect("src/owner.ts");
    expect(owner == resident.inspect("src/owner.ts"), "relation cold/warm parity");
    const auto& relations = owner.at("relations");
    expect(relations.at("complete") == true && relations.at("compiler_semantics") == false, "relation interpretation");
    bool outgoing = false, incoming = false;
    for (const auto& row : relations.at("items")) {
        expect(row.at("path") != "src/missing.ts", "unresolved is not a navigation edge");
        if (row.at("path") == "src/dependency.ts") {
            outgoing = row.at("direction") == "outgoing" && row.at("source_line") == 1;
        }
        if (row.at("path") == "README.md") incoming = row.at("direction") == "incoming";
    }
    expect(outgoing && incoming, "declared imports and document links visible: " + relations.dump());
    expect(disk.inspect("src/absent.cpp").at("relations").at("total") == 0, "absent path has no guessed links");
}

void inspect_relation_limits(const fs::path& parent) {
    Fixture fixture(parent);
    fixture.options.profile.source_fact_extensions = {".ts"};
    std::string source;
    for (int i = 0; i < 30; ++i) {
        const auto name = "unit" + std::to_string(i) + ".ts";
        source += "import './" + name + "';\n";
        fixture.write("src/" + name, "export const unit = 0;\n");
    }
    fixture.write("src/owner.ts", source);
    fixture.publish();
    RetrievalSession session(fixture.options, true);
    const auto result = session.inspect("src/owner.ts");
    const auto& links = result.at("relations");
    expect(links.at("total") == 30 && links.at("returned") == 16 && links.at("omitted") == 14 &&
           links.at("complete") == false, "link count cap is explicit");
    expect(links.dump(2).size() <= 8192 && result.dump(2).size() + 1 <= 65536, "link and envelope byte caps");
    Fixture wide(parent);
    wide.options.profile.source_fact_extensions = {".ts"};
    source.clear();
    const std::string prefix = std::string(180, 'a') + "/" + std::string(180, 'b') + "/";
    for (int i = 0; i < 20; ++i) {
        const auto name = prefix + "unit" + std::to_string(i) + ".ts";
        source += "import './" + name + "';\n";
        wide.write("src/" + name, "export const unit = 0;\n");
    }
    wide.write("src/owner.ts", source);
    wide.publish();
    RetrievalSession wide_session(wide.options, true);
    const auto bounded = wide_session.inspect("src/owner.ts");
    const auto& wide_links = bounded.at("relations");
    expect(wide_links.at("total") == 20 && wide_links.at("returned").get<int>() < 16 &&
           wide_links.at("complete") == false && wide_links.dump(2).size() <= 8192,
           "wide relation records honor separate byte budget");
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected fixture parent");
        shin::context::inspect_checks(argv[1]);
        shin::context::inspect_limits(argv[1]);
        shin::context::inspect_relations_checks(argv[1]);
        shin::context::inspect_relation_limits(argv[1]);
        std::cout << "inspect: inventory, admission, immutable parity and bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
