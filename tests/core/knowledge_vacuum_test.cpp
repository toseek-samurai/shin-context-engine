#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"

#include <atomic>
#include <chrono>
#include <iostream>

namespace fs = std::filesystem;

namespace shin::context {
namespace {

class TempDirectory {
public:
    explicit TempDirectory(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent / ("shin-knowledge-vacuum-" + std::to_string(stamp) + "." +
                          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("knowledge vacuum test: " + message);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

void write_text(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) fail("cannot create fixture file " + path.string());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) fail("cannot write fixture file " + path.string());
}

Profile fixture_profile() {
    Profile profile;
    profile.id = "knowledge-vacuum";
    profile.source_label = "knowledge vacuum fixture";
    profile.config_hash = "knowledge-vacuum-test-profile-v1";
    profile.root_files = {"README.md"};
    return profile;
}

RuntimeOptions make_fixture_options(const fs::path& base) {
    RuntimeOptions options;
    options.root = base / "root";
    options.out_root = base / "index";
    options.profile = fixture_profile();
    options.force_index = true;
    fs::create_directories(options.root);
    return options;
}

void write_healthy_sources(const RuntimeOptions& options) {
    write_text(options.root / "README.md",
               "# Knowledge Vacuum Fixture\n\n"
               "This source produces a healthy generated snapshot.\n");
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Health\n\n"
               "A background assessment compares the published and expected snapshots.\n");
}

void publish_generation(const RuntimeOptions& options) {
    const GeneratedOutput generated = generate_outputs(options.profile, options.root);
    shin::generated_io::IndexLock writer_lock(
        options.out_root, shin::generated_io::LockMode::exclusive,
        shin::generated_io::LockTarget::writer);
    shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
}

std::map<std::string, std::string> generated_tree(const RuntimeOptions& options) {
    std::map<std::string, std::string> result;
    const fs::path root = options.out_root / kGeneratedDir;
    for (const fs::directory_entry& entry : fs::directory_iterator(root)) {
        expect(entry.is_regular_file(), "generated fixture contains a non-regular entry");
        result.emplace(entry.path().filename().generic_string(), read_file(entry.path()));
    }
    return result;
}

VacuumAssessment inspect_with_read_lock(const RuntimeOptions& options) {
    shin::generated_io::IndexLock read_lock(
        options.out_root, shin::generated_io::LockMode::shared_existing,
        shin::generated_io::LockTarget::index);
    return inspect_generated_kb_vacuum(options);
}

void expect_non_mutating_assessment(const RuntimeOptions& options,
                                    const std::function<void(const VacuumAssessment&)>& inspect) {
    const auto before = generated_tree(options);
    const VacuumAssessment assessment = inspect_with_read_lock(options);
    inspect(assessment);
    expect(generated_tree(options) == before, "assessment mutated generated output");
}

void check_healthy_snapshot(const RuntimeOptions& options) {
    const std::string published_snapshot_id = GeneratedSnapshot::load(options)->id();
    expect_non_mutating_assessment(options, [&](const VacuumAssessment& assessment) {
        expect(assessment.summary.expected_files > 0, "healthy snapshot expected no files");
        expect(assessment.summary.existing_files == assessment.summary.expected_files,
               "healthy snapshot existing file count differs");
        expect(assessment.summary.current_files == assessment.summary.expected_files,
               "healthy snapshot current file count differs");
        expect(!assessment.summary.changes_needed && !assessment.summary.changes_remaining,
               "healthy snapshot reports pending changes");
        expect(assessment.summary.broken_local_links == 0, "healthy snapshot reports broken links");
        expect(assessment.expected_snapshot_id == published_snapshot_id,
               "expected snapshot id does not match GeneratedSnapshot id");
    });
}

void check_source_drift(const RuntimeOptions& options) {
    const std::string published_snapshot_id = GeneratedSnapshot::load(options)->id();
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Health\n\n"
               "The source changed after the published snapshot was created.\n");
    expect_non_mutating_assessment(options, [&](const VacuumAssessment& assessment) {
        expect(assessment.summary.stale_files > 0, "source drift did not report stale generated files");
        expect(assessment.summary.changes_needed && assessment.summary.changes_remaining,
               "source drift did not remain read-only");
        expect(assessment.expected_snapshot_id != published_snapshot_id,
               "source drift did not change the expected snapshot id");
    });
}

void check_orphan_generated_file(const RuntimeOptions& options) {
    publish_generation(options);
    write_text(options.out_root / kGeneratedDir / "orphan.jsonl", "orphan\n");
    expect_non_mutating_assessment(options, [&](const VacuumAssessment& assessment) {
        expect(assessment.summary.orphan_files == 1, "orphan generated file was not reported");
        expect(assessment.summary.changes_needed && assessment.summary.changes_remaining,
               "orphan generated file did not remain planned");
    });
    std::error_code error;
    fs::remove(options.out_root / kGeneratedDir / "orphan.jsonl", error);
    if (error) fail("cannot remove orphan fixture file");
}

void check_broken_link(const RuntimeOptions& options) {
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Health\n\n"
               "[Missing local document](missing.md)\n");
    expect_non_mutating_assessment(options, [&](const VacuumAssessment& assessment) {
        expect(assessment.summary.broken_local_links == 1, "broken local link was not reported");
        expect(assessment.summary.changes_needed && assessment.summary.changes_remaining,
               "broken local link did not remain read-only");
    });
}

void check_publication_in_progress_rejected(const RuntimeOptions& options) {
    const fs::path marker = options.out_root / "docs/kb/.shin-context-publishing";
    write_text(marker, "fixture\n");
    try {
        shin::generated_io::IndexLock read_lock(
            options.out_root, shin::generated_io::LockMode::shared_existing,
            shin::generated_io::LockTarget::index);
        (void)inspect_generated_kb_vacuum(options);
        fail("publication in progress was accepted");
    } catch (const CliError& error) {
        expect(error.code() == "publication-incomplete", "unexpected publication rejection code");
    }
    std::error_code error;
    fs::remove(marker, error);
    if (error) fail("cannot remove publication fixture marker");
}

void check_streamed_comparison(const fs::path& base) {
    const auto options = make_fixture_options(base);
    std::string source = "# Chunk Boundary Fixture\n";
    for (int i = 0; i < 1200; ++i)
        source += "\n## tokenBoundaryItem" + std::to_string(i) + "\n\nUnique indexed text.\n";
    write_text(options.root / "README.md", source);
    publish_generation(options);
    const auto file = options.out_root / kGeneratedDir / "terms.lex";
    const auto expected = read_file(file);
    expect(expected.size() > 2 * 65536, "fixture must cross two comparison blocks");
    const auto check = [&] {
        std::ostringstream captured;
        struct Restore {
            std::streambuf* previous;
            ~Restore() { std::cout.rdbuf(previous); }
        } restore{std::cout.rdbuf(captured.rdbuf())};
        return run_check(options);
    };
    expect(check() == 0, "healthy multi-block check failed");
    const auto reject = [&](const std::string& changed) {
        write_text(file, changed);
        expect(check() == 1, "check missed generated byte drift");
        const auto assessment = inspect_with_read_lock(options);
        expect(assessment.summary.stale_files == 1, "vacuum missed generated byte drift");
        expect(read_file(file) == changed, "read-only comparison changed the file");
    };
    for (const std::size_t offset : {std::size_t{0}, std::size_t{65535}, std::size_t{65536}, expected.size() - 1}) {
        auto changed = expected;
        changed[offset] = changed[offset] == 'x' ? 'y' : 'x';
        reject(changed);
    }
    for (const auto size : {std::size_t{0}, std::size_t{65536}, expected.size() - 1})
        reject(expected.substr(0, size));
    reject(expected + "extra");
    write_text(file, expected);
    expect(check() == 0, "restored multi-block check failed");
}

int run_knowledge_vacuum_test(const fs::path& temporary_parent) {
    TempDirectory temporary(temporary_parent);
    RuntimeOptions options = make_fixture_options(temporary.path());
    write_healthy_sources(options);
    publish_generation(options);
    check_healthy_snapshot(options);
    check_source_drift(options);
    check_orphan_generated_file(options);
    check_broken_link(options);
    check_publication_in_progress_rejected(options);
    check_streamed_comparison(temporary.path() / "streamed");
    return EXIT_SUCCESS;
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc > 2) {
            std::cerr << "usage: knowledge-vacuum-test [temporary-parent]\n";
            return EXIT_FAILURE;
        }
        const fs::path parent = argc == 2 ? fs::absolute(argv[1]) : fs::temp_directory_path();
        if (!fs::is_directory(parent)) {
            std::cerr << "temporary parent is not a directory: " << parent << '\n';
            return EXIT_FAILURE;
        }
        return shin::context::run_knowledge_vacuum_test(parent);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
