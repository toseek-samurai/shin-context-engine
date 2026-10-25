#include "context_internal.h"
#include "worktree_state.h"

#include <chrono>

namespace shin::context {
namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("source-observation: " + message);
}

class Fixture {
public:
    explicit Fixture(const fs::path& parent)
        : root(fs::absolute(parent / ("source-observation-" + std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count())))) {
        fs::create_directories(root / "src" / "nested");
        std::ofstream(root / "src" / "nested" / "picked.cpp") << "int picked;\n";
        std::ofstream(root / "src" / "nested" / "ignored.pending") << "temporary\n";
        std::ofstream(root / "src" / "ignored.txt") << "text\n";
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path root;
};

void enumeration_is_lexical_and_keeps_coverage(const fs::path& parent) {
    Fixture fixture(parent);
    Profile profile;
    profile.source_fact_roots = {"src"};
    profile.source_fact_extensions = {".cpp"};
    fs::create_directories(fixture.root / "docs");
    std::ofstream(fixture.root / "docs/guide.md") << "# Guide\n";
    profile.source_extensions = {".md"};
    expect(enumerate_source_paths(profile, fixture.root / ".") == enumerate_source_paths(profile, fixture.root),
           "lexical document inventory preserves equivalent root spelling");
    const SourceFactPathResult result = enumerate_source_fact_paths(profile, fixture.root);
    expect(result.paths == std::vector<std::string>{"src/nested/picked.cpp"},
           "only selected extension is inventoried");
    expect(result.candidate_file_count == 1 && result.capped_file_count == 0,
           "inventory counts remain complete");

    const fs::path outside = fixture.root.parent_path() / (fixture.root.filename().string() + "-outside");
    fs::create_directories(outside);
    std::ofstream(outside / "linked.cpp") << "int linked;\n";
    std::error_code symlink_ec;
    fs::create_directory_symlink(outside, fixture.root / "src" / "linked", symlink_ec);
    if (!symlink_ec) {
        const SourceFactPathResult with_link = enumerate_source_fact_paths(profile, fixture.root);
        expect(with_link.paths == result.paths, "linked directories remain outside source coverage");
    }
    std::error_code cleanup_ec;
    fs::remove_all(outside, cleanup_ec);
}

void explicit_document_aliases_preserve_selection(const fs::path& parent) {
    Fixture fixture(parent);
    fs::create_directory(fixture.root / "notes");
    std::ofstream(fixture.root / "notes/first.md") << "# Same bytes\n";
    std::ofstream(fixture.root / "notes/distinct.md") << "# Same bytes\n";
    fs::create_hard_link(fixture.root / "notes/first.md", fixture.root / "notes/alias.md");
    Profile profile;
    profile.source_extensions = {".md"};
    profile.root_files = {"notes/first.md", "notes/first.md", "notes/distinct.md"};
    profile.optional_root_files = {"notes/alias.md", "notes/missing.md"};
    expect(enumerate_source_paths(profile, fixture.root) ==
           std::vector<std::string>{"notes/distinct.md", "notes/first.md"},
           "explicit aliases deduplicate by file identity, never content or list membership");
    profile.root_files = {"notes/alias.md", "notes/distinct.md"};
    profile.optional_root_files = {"notes/first.md"};
    expect(enumerate_source_paths(profile, fixture.root) ==
           std::vector<std::string>{"notes/alias.md", "notes/distinct.md"},
           "first explicit spelling remains the selected alias");
    std::error_code symlink_error;
    fs::create_symlink(fixture.root / "notes/distinct.md", fixture.root / "notes/symlink.md", symlink_error);
    if (!symlink_error) {
        profile.root_files.push_back("notes/symlink.md");
        expect(enumerate_source_paths(profile, fixture.root).size() == 2,
               "symlink alias is not admitted");
    }
    // Each invocation observes identities anew, including replacements.
    fs::remove(fixture.root / "notes/first.md");
    std::ofstream(fixture.root / "notes/first.md") << "# Replacement\n";
    expect(enumerate_source_paths(profile, fixture.root).size() == 3,
           "replacement file identity must not reuse an earlier alias observation");
}

void explicit_document_inventory_at_scale(const fs::path& parent) {
    Fixture fixture(parent);
    fs::create_directory(fixture.root / "notes");
    Profile profile;
    std::vector<std::string> expected;
    for (unsigned i = 0; i < 2048; ++i) {
        const auto path = "notes/entry-" + std::to_string(i) + ".md";
        std::ofstream(fixture.root / path) << "# Inventory\n";
        profile.root_files.push_back(path);
        expected.push_back(path);
    }
    std::sort(expected.begin(), expected.end());
    expect(enumerate_source_paths(profile, fixture.root) == expected,
           "large explicit inventory retains every selected file exactly once");
}

void errors_are_typed_and_safe() {
    for (const auto& [error, expected_code, expected_category] : {
             std::tuple{std::make_error_code(std::errc::no_such_file_or_directory),
                        "file-changed-during-verification", "entry-disappeared"},
             std::tuple{std::make_error_code(std::errc::permission_denied),
                        "access-denied", "access-denied"},
             std::tuple{std::make_error_code(std::errc::io_error),
                        "source-io-error", "io-error"}}) {
        try {
            throw_source_observation_error("read-source-metadata", error);
        } catch (const CliError& caught) {
            expect(caught.code() == expected_code, "classified error code");
            expect(caught.diagnostic().at("schema_version") == "shin.source-observation-diagnostic.v1",
                   "diagnostic schema");
            expect(caught.diagnostic().at("stage") == "source-inventory", "diagnostic stage");
            expect(caught.diagnostic().at("operation") == "read-source-metadata",
                   "diagnostic operation");
            expect(caught.diagnostic().at("category") == expected_category,
                   "diagnostic category");
            expect(caught.what() == std::string("A source entry disappeared during freshness verification.") ||
                   caught.what() == std::string("Access to a source entry was denied.") ||
                   caught.what() == std::string("Source filesystem observation failed."),
                   "diagnostic exposes no source path or OS error text");
            continue;
        }
        throw std::runtime_error("source-observation: expected typed filesystem error");
    }
}

void disappeared_selected_source_is_changed(const fs::path& parent) {
    Fixture fixture(parent);
    const fs::path selected = fixture.root / "src" / "selected.cpp";
    try {
        (void)source_fingerprint_file(selected, "read-source-content");
    } catch (const CliError& error) {
        expect(error.code() == "file-changed-during-verification",
               "disappeared selected file is reported as changed");
        expect(error.diagnostic().at("category") == "entry-disappeared",
               "missing source diagnostic category");
        expect(std::string(error.what()).find(selected.string()) == std::string::npos,
               "safe source message omits path");
        return;
    }
    throw std::runtime_error("source-observation: expected selected source refusal");
}

void changed_default_budgets_invalidate_unchanged_profile(const fs::path& parent) {
    Fixture fixture(parent);
    RuntimeOptions options;
    options.root = fixture.root;
    options.profile.id = "budget-fixture";
    options.profile.config_hash = "fnv1a64:0123456789abcdef";
    WorktreeInventory inventory;
    Json manifest{{"profile", options.profile.id}, {"source_count", 0},
                  {"sources", Json::array()}, {"source_fact_sources", Json::array()},
                  {"source_fact_counts", {{"candidate_files", 0}, {"capped_files", 0}}},
                  {"generator_version", kGeneratorVersion},
                  {"profile_config_hash", profile_config_hash(options.profile)},
                  {"profile_config", manifest_profile_config_identity(options.profile)},
                  {"source_fact_limits", {{"max_files", 2000}, {"max_file_bytes", 524288}}}};
    const auto changed = compare_worktree(options, manifest, inventory, 10);
    expect(changed.at("current") == false && changed.at("summary").at("metadata_changed") == 1,
           "changed Core defaults require publication even with identical profile bytes and source inventory");
    expect(changed.at("details").at(0).at("key") == "source_fact_limits", "effective budget drift is identified");
    manifest["source_fact_limits"] = {{"max_files", options.profile.source_fact_max_files},
                                     {"max_file_bytes", options.profile.source_fact_max_file_bytes}};
    expect(compare_worktree(options, manifest, inventory, 10).at("current") == true,
           "matching effective budgets keep an otherwise identical generation current");
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected fixture parent");
        shin::context::enumeration_is_lexical_and_keeps_coverage(argv[1]);
        shin::context::explicit_document_aliases_preserve_selection(argv[1]);
        shin::context::explicit_document_inventory_at_scale(argv[1]);
        shin::context::errors_are_typed_and_safe();
        shin::context::disappeared_selected_source_is_changed(argv[1]);
        shin::context::changed_default_budgets_invalidate_unchanged_profile(argv[1]);
        std::cout << "source-observation: lexical inventory and typed errors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
