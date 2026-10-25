#include "context_internal.h"
#include "generation_admission.h"
#include "snapshot_verification.h"
#include "generated_io.h"

#include <chrono>

namespace {
using namespace shin::context;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value;
    if (!stream) throw std::runtime_error("fixture write failed");
}
template<class Action> void refuses(Action action) {
    try { action(); }
    catch (const CliError&) { return; }
    throw std::runtime_error("changed generation accepted");
}

void test(const fs::path& base, const fs::path& registry) {
    RuntimeOptions options;
    options.root = base / "root";
    options.out_root = base / "index";
    options.profile_dir = base / "profiles";
    auto profile = parse_json_text(read_file(registry / "generic-source.json"), "fixture");
    profile["sourceExtensions"] = {".md"};
    profile["sourceFactRoots"] = {"src", "apps"};
    profile["sourceFactExtensions"] = {".js", ".rsx"};
    write(options.profile_dir / "generic-source.json", profile.dump());
    options.profile = make_profile("generic-source", options.profile_dir);
    write(options.root / "README.md", "# Admission\n\n[linked](target.bin)\n");
    write(options.root / "target.bin", "present");
    write(options.root / "src/value.js", "export function value() { return 7; }\n");
    write(options.root / "apps/CRM/screen/main.rsx", "<App><Text id=\"label\" /></App>\n");
    const auto output = generate_outputs(options.profile, options.root);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, output.files);
    const auto lock = lock_index_for_read(options.out_root);
    const auto certificate = generation_fingerprint(options);
    expect(assess_index_freshness(options, true).current, "qualification must be exact");
    options.generation_admission = GenerationAdmission::reuse(options, certificate);
    expect(bool(options.generation_admission), "matching generation was not admitted");
    const auto proof = options.generation_admission;
    const auto count = generation_execution_count();
    expect(assess_index_freshness(options, true).current, "strict request refused");
    proof->verify_content(options);
    expect(count == generation_execution_count(), "reuse regenerated output");
    expect(!GenerationAdmission::reuse(options, "fnv1a64:0000000000000000"), "wrong receipt accepted");
    expect(run_check(options) == 0, "explicit check lost its audit");
    expect(run_vacuum(options, {"--strict"}) == 0, "explicit vacuum lost its audit");
    expect(generation_execution_count() == count + 2, "audit commands reused the receipt");

    const auto verified = load_verified_snapshot(options.out_root);
    auto reused = GenerationAdmission::reuse(options, certificate, &verified);
    expect(bool(reused) && reused->fingerprint() == proof->fingerprint(), "observed byte reuse changed admission");
    expect(reused->reused_output_files() == expected_generated_relative_files(options).size() &&
           reused->reused_output_bytes() > 0, "verified output byte reuse was not measured");
    reused->verify_content(options);
    auto incomplete = verified; incomplete.fingerprints.erase(incomplete.fingerprints.begin());
    refuses([&] { GenerationAdmission::reuse(options, certificate, &incomplete); });
    auto foreign_snapshot = verified; foreign_snapshot.out_root = base / "foreign-index";
    refuses([&] { GenerationAdmission::reuse(options, certificate, &foreign_snapshot); });
    expect(!GenerationAdmission::reuse(options, "fnv1a64:0000000000000000", &verified), "wrong receipt reused observed bytes");
    RuntimeOptions destination = options;
    destination.out_root = base / "published";
    destination.generation_admission.reset();
    shin::generated_io::publish_snapshot(destination.out_root, kGeneratedDir, output.files);
    const auto destination_lock = lock_index_for_read(destination.out_root);
    const auto before_copy = generation_execution_count();
    auto copy = proof->published_copy(options, destination);
    destination.generation_admission = copy;
    expect(assess_index_freshness(destination, true).current, "published copy not admitted");
    expect(copy->fingerprint() != proof->fingerprint(), "destination identity was not rebound");
    expect(generation_execution_count() == before_copy, "copy transfer regenerated output");
    std::set<fs::path> expected_files;
    const auto inventory = observe_worktree(options);
    for (const auto* members : {&inventory.documents, &inventory.source_facts})
        for (const auto& [path, state] : *members) {
            (void)state;
            expected_files.insert(options.root / path);
        }
    const auto source_files = expected_files.size();
    for (const auto& path : expected_generated_relative_files(options)) {
        expected_files.insert(options.out_root / path);
        expected_files.insert(destination.out_root / path);
    }
    expect(source_files > 0 && proof->verify_content_together(options, *copy, destination) == expected_files.size(),
           "joint verification must read shared sources once and both output trees");
    expect(proof->verify_content_together(options, *proof, options) ==
           source_files + expected_generated_relative_files(options).size(), "same closure was read twice");
    RuntimeOptions misbound = destination;
    misbound.out_root = base / "foreign-index";
    refuses([&] { proof->verify_content_together(options, *copy, misbound); });
    refuses([&] { copy->verify_content_together(misbound, *proof, options); });

    auto foreign_copy = destination;
    foreign_copy.root = base / "joint-foreign-root";
    fs::copy(options.root, foreign_copy.root, fs::copy_options::recursive);
    const auto foreign_proof = GenerationAdmission::reuse(foreign_copy, generation_fingerprint(foreign_copy));
    expect(foreign_proof && foreign_proof->assess(foreign_copy).current,
           "foreign fixture must be independently admitted");
    refuses([&] { proof->verify_content_together(options, *foreign_proof, foreign_copy); });

    // A shared read must retain every input and both output boundaries. Renew
    // admissions after restoring a fixture so metadata checks cannot mask gaps.
    const auto joint_negative = [&](auto mutate, auto restore) {
        const auto left = GenerationAdmission::reuse(options, certificate);
        const auto right = left->published_copy(options, destination);
        mutate();
        refuses([&] { left->verify_content_together(options, *right, destination); });
        refuses([&] { left->published_copy(options, destination); });
        restore();
    };
    for (const auto& path : {options.root / "src/value.js",
                            options.out_root / kGeneratedDir / "documents.jsonl",
                            destination.out_root / kGeneratedDir / "documents.jsonl"}) {
        const auto bytes = read_file(path);
        const auto mtime = fs::last_write_time(path);
        auto edited = bytes; edited[0] = edited[0] == 'x' ? 'y' : 'x';
        joint_negative([&] { write(path, edited); fs::last_write_time(path, mtime); },
                       [&] { write(path, bytes); });
    }
    joint_negative([&] { write(options.root / "src/added.js", "export const x = 1;\n"); },
                   [&] { fs::remove(options.root / "src/added.js"); });
    joint_negative([&] { fs::remove(options.root / "target.bin"); },
                   [&] { write(options.root / "target.bin", "present"); });
    joint_negative([&] { write(options.root / "apps/CRM/metadata.json", "{}"); },
                   [&] { fs::remove(options.root / "apps/CRM/metadata.json"); });
    joint_negative([&] { write(destination.out_root / kGeneratedDir / "extra.json", "{}"); },
                   [&] { fs::remove(destination.out_root / kGeneratedDir / "extra.json"); });
    joint_negative([&] {
        auto edited = profile; edited["summary"] = "different policy";
        write(options.profile_dir / "generic-source.json", edited.dump());
    }, [&] { write(options.profile_dir / "generic-source.json", profile.dump()); });
    const auto copied_document = destination.out_root / kGeneratedDir / "documents.jsonl";
    const auto copied_bytes = read_file(copied_document);
    write(copied_document, copied_bytes + " ");
    refuses([&] { proof->published_copy(options, destination); });
    write(copied_document, copied_bytes);
    fs::remove(options.root / "target.bin");
    refuses([&] { proof->published_copy(options, destination); });
    write(options.root / "target.bin", "present");
    write(options.root / "apps/CRM/metadata.json", "{}");
    refuses([&] { proof->published_copy(options, destination); });
    fs::remove(options.root / "apps/CRM/metadata.json");
    RuntimeOptions different_root = destination;
    different_root.root = base / "foreign-root";
    fs::copy(options.root, different_root.root, fs::copy_options::recursive);
    refuses([&] { proof->published_copy(options, different_root); });

    const auto reuse_target = options.out_root / kGeneratedDir / "documents.jsonl";
    const auto reuse_bytes = read_file(reuse_target);
    const auto reuse_mtime = fs::last_write_time(reuse_target);
    auto changed_bytes = reuse_bytes; changed_bytes[0] = changed_bytes[0] == '{' ? '[' : '{';
    write(reuse_target, changed_bytes); fs::last_write_time(reuse_target, reuse_mtime);
    refuses([&] { GenerationAdmission::reuse(options, certificate, &verified); });
    refuses([&] { reused->verify_content(options); });
    write(reuse_target, reuse_bytes);
    const auto restored_snapshot = load_verified_snapshot(options.out_root);
    expect(bool(GenerationAdmission::reuse(options, certificate, &restored_snapshot)), "restored byte reuse refused");
    const auto reuse_source = options.root / "src/value.js";
    const auto reuse_source_bytes = read_file(reuse_source);
    write(reuse_source, "export function value() { return 9; }\n");
    expect(!GenerationAdmission::reuse(options, certificate, &restored_snapshot), "output observation bypassed live sources");
    write(reuse_source, reuse_source_bytes);

    // Direct byte verification still fences membership and non-content
    // dependencies; each negative starts from a newly valid admission.
    auto direct = GenerationAdmission::reuse(options, certificate);
    expect(bool(direct), "direct verification baseline missing");
    direct->verify_content(options);
    write(options.root / "src/new.js", "export const added = 1;\n");
    refuses([&] { direct->verify_content(options); });
    fs::remove(options.root / "src/new.js");
    direct = GenerationAdmission::reuse(options, certificate);
    fs::remove(options.root / "target.bin");
    refuses([&] { direct->verify_content(options); });
    write(options.root / "target.bin", "present");
    direct = GenerationAdmission::reuse(options, certificate);
    write(options.root / "apps/CRM/metadata.json", "{}");
    refuses([&] { direct->verify_content(options); });
    fs::remove(options.root / "apps/CRM/metadata.json");

    const auto source = options.root / "src/value.js";
    const auto original = read_file(source);
    const auto modified = fs::last_write_time(source);
    write(source, "export function value() { return 8; }\n");
    fs::last_write_time(source, modified);
    refuses([&] { proof->assess(options); });
    refuses([&] { proof->verify_content(options); });
    expect(!GenerationAdmission::reuse(options, certificate), "same-size source edit accepted");
    refuses([&] { proof->published_copy(options, destination); });
    write(source, original);
    expect(bool(GenerationAdmission::reuse(options, certificate)), "restored content cannot be readmitted");

    fs::remove(options.root / "target.bin");
    expect(!GenerationAdmission::reuse(options, certificate), "unindexed link target ignored");
    write(options.root / "target.bin", "present");

    write(options.root / "apps/CRM/metadata.json", "{}");
    expect(!GenerationAdmission::reuse(options, certificate), "unindexed Retool owner ignored");
    fs::remove(options.root / "apps/CRM/metadata.json");
    fs::create_directory(options.root / "apps/CRM/screen/.positions");
    expect(!GenerationAdmission::reuse(options, certificate), "Retool layout directory ignored");
    fs::remove(options.root / "apps/CRM/screen/.positions");

    const auto extra = options.out_root / kGeneratedDir / "extra.json";
    write(extra, "{}");
    refuses([&] { GenerationAdmission::reuse(options, certificate); });
    fs::remove(extra);
    const auto artifact = options.out_root / kGeneratedDir / "documents.jsonl";
    const auto bytes = read_file(artifact);
    const auto artifact_time = fs::last_write_time(artifact);
    auto fresh = GenerationAdmission::reuse(options, certificate);
    write(artifact, bytes + " ");
    fs::last_write_time(artifact, artifact_time);
    refuses([&] { fresh->assess(options); });
    refuses([&] { fresh->verify_content(options); });
    expect(!GenerationAdmission::reuse(options, certificate), "output edit accepted");
    fs::remove(artifact);
    refuses([&] { GenerationAdmission::reuse(options, certificate); });
    fs::create_symlink(source, artifact);
    refuses([&] { GenerationAdmission::reuse(options, certificate); });
    fs::remove(artifact);
    write(artifact, bytes);

    const auto manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    const auto manifest_bytes = read_file(manifest_path);
    auto manifest = parse_json_text(manifest_bytes, "fixture manifest");
    write(artifact, bytes + " ");
    for (auto& row : manifest["generated_file_integrity"]["files"]) {
        if (row["path"] == (kGeneratedDir / "documents.jsonl").generic_string()) {
            row["bytes"] = bytes.size() + 1;
            row["content_hash"] = fnv1a64(bytes + " ");
        }
    }
    write(manifest_path, manifest.dump());
    expect(!GenerationAdmission::reuse(options, certificate), "self-consistent forged output accepted");
    write(artifact, bytes);
    write(manifest_path, manifest_bytes);

    RuntimeOptions foreign = options;
    foreign.root = base / "other";
    refuses([&] { proof->assess(foreign); });
    refuses([&] { proof->verify_content(foreign); });
    profile["summary"] = "changed policy";
    write(options.profile_dir / "generic-source.json", profile.dump());
    refuses([&] { proof->assess(options); });
    refuses([&] { proof->published_copy(options, destination); });
    refuses([&] { proof->verify_content(options); });
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto base = std::filesystem::temp_directory_path() /
        ("generation-admission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        test(base, argv[1]);
        std::filesystem::remove_all(base);
        std::cout << "generation admission boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
