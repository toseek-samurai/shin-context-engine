#include "context_internal.h"
#include "validation_generation.h"
#include "generated_io.h"
#include <chrono>

namespace {
using namespace shin::context;
void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path()); std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text; if (!out) throw std::runtime_error("fixture write failed");
}
template<class Action> std::pair<int, Json> capture(Action action) {
    std::ostringstream out; auto* previous = std::cout.rdbuf(out.rdbuf());
    int code;
    try { code = action(); } catch (...) { std::cout.rdbuf(previous); throw; }
    std::cout.rdbuf(previous);
    return {code, parse_json_text(out.str(), "command result")};
}
void test(const fs::path& base, const fs::path& registry) {
    RuntimeOptions options;
    options.root = base / "root"; options.out_root = base / "index"; options.profile_dir = base / "profiles";
    auto profile = parse_json_text(read_file(registry / "generic-source.json"), "profile");
    profile["sourceExtensions"] = {".md"}; profile["sourceFactRoots"] = {"src", "apps"};
    profile["sourceFactExtensions"] = {".js", ".rsx"};
    write(options.profile_dir / "generic-source.json", profile.dump());
    options.profile = make_profile("generic-source", options.profile_dir);
    write(options.root / "README.md", "# Validation\n\n[dependency](target.bin)\n");
    write(options.root / "target.bin", "present");
    write(options.root / "src/value.js", "export function value() { return 7; }\n");
    write(options.root / "apps/CRM/screen/main.rsx", "<App><Text id=\"label\" /></App>\n");
    const auto expected = generate_outputs(options.profile, options.root);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, expected.files);
    const auto lock = lock_index_for_read(options.out_root);
    auto cache = std::make_shared<ValidationGeneration>(options);
    auto before = generation_execution_count();
    auto first = cache->acquire(options, true);
    expect(first && first->files == expected.files, "fresh expectation differs");
    expect(cache->acquire(options, true) == first, "unchanged inputs not shared");
    expect(generation_execution_count() == before + 1 && cache->metrics().at("reuses") == 1,
           "shared preparation regenerated");
    auto direct_before = generation_execution_count();
    expect(capture([&] { return run_check(options); }).first == 0, "direct check failed");
    expect(capture([&] { return run_vacuum(options, {"--strict"}); }).first == 0, "direct vacuum failed");
    expect(generation_execution_count() == direct_before + 2, "standalone audit reused preparation");
    before = generation_execution_count();
    const auto validation = capture([&] { return run_validate(options, options.profile_dir, {"--strict"}); });
    expect(validation.second.at("gates").size() == 7 && validation.second.at("summary").at("attempted") == 6,
           "aggregate validation skipped gates");
    expect(generation_execution_count() == before + 1 && validation.second.at("preparation").at("reuses") == 3,
           "aggregate strict validation did not share its expected generation");
    for (const auto& [path, bytes] : expected.files)
        expect(read_file(options.out_root / path) == bytes, "validation mutated the index");

    const auto invalidates = [&](auto mutate, auto restore) {
        auto current = std::make_shared<ValidationGeneration>(options);
        if (!current->acquire(options, true))
            throw std::runtime_error("negative baseline not reusable: " + current->metrics().dump());
        mutate();
        expect(!current->acquire(options, true) && current->metrics().at("invalidations") == 1,
               "changed dependencies reused old generation");
        auto fallback = options; fallback.validation_generation = current;
        expect(validation_generated_output(fallback)->files == generate_outputs(options.profile, options.root).files,
               "fallback diverged from independent generation");
        restore();
    };
    const auto source = options.root / "src/value.js"; const auto bytes = read_file(source);
    const auto mtime = fs::last_write_time(source);
    invalidates([&] { write(source, "export function value() { return 8; }\n"); fs::last_write_time(source, mtime); },
                [&] { write(source, bytes); });
    invalidates([&] { write(options.root / "src/added.js", "export const added = 1;\n"); },
                [&] { fs::remove(options.root / "src/added.js"); });
    invalidates([&] { fs::remove(source); }, [&] { write(source, bytes); });
    invalidates([&] { fs::rename(source, options.root / "src/renamed.js"); },
                [&] { fs::rename(options.root / "src/renamed.js", source); });
    invalidates([&] { fs::remove(options.root / "target.bin"); },
                [&] { write(options.root / "target.bin", "present"); });
    invalidates([&] { write(options.root / "apps/CRM/metadata.json", "{}"); },
                [&] { fs::remove(options.root / "apps/CRM/metadata.json"); });
    invalidates([&] { fs::create_directory(options.root / "apps/CRM/screen/.positions"); },
                [&] { fs::remove(options.root / "apps/CRM/screen/.positions"); });
    invalidates([&] { auto edited = profile; edited["summary"] = "new policy";
                     write(options.profile_dir / "generic-source.json", edited.dump()); },
                [&] { write(options.profile_dir / "generic-source.json", profile.dump()); });

    // A dependency newly added to source is absent from the published index.
    const auto readme = read_file(options.root / "README.md");
    write(options.root / "README.md", readme + "\n[new](new-target.bin)\n");
    invalidates([&] { write(options.root / "new-target.bin", "appeared"); },
                [&] { fs::remove(options.root / "new-target.bin"); });
    write(options.root / "README.md", readme);

    auto fresh = std::make_shared<ValidationGeneration>(options);
    expect(bool(fresh->acquire(options, true)), "binding baseline");
    auto foreign = options; foreign.root = base / "foreign-root";
    expect(!fresh->acquire(foreign, true), "foreign root reused");
    fresh = std::make_shared<ValidationGeneration>(options); fresh->acquire(options, true);
    foreign = options; foreign.out_root = base / "foreign-index";
    expect(!fresh->acquire(foreign, true), "foreign output reused");
    fresh = std::make_shared<ValidationGeneration>(options); fresh->acquire(options, true);
    expect(!fresh->acquire(options, false), "different lexical format reused");
    fresh = std::make_shared<ValidationGeneration>(options); fresh->acquire(options, true);
    foreign = options; foreign.profile.default_topic += "-changed-in-memory";
    expect(!fresh->acquire(foreign, true), "mutated effective profile reused cached config hash");
    ValidationGeneration tiny(options, 1);
    expect(bool(tiny.acquire(options, true)) && !tiny.acquire(options, true) &&
           tiny.metrics().at("retained_bytes") == 0 && tiny.metrics().at("fallback_reason") == "retained-byte-limit",
           "oversized result retained");

    // Reusing expected bytes never certifies the actual published bytes.
    options.validation_generation = std::make_shared<ValidationGeneration>(options);
    expect(capture([&] { return run_check(options); }).first == 0, "initial cached check");
    const auto artifact = options.out_root / kGeneratedDir / "documents.jsonl";
    const auto original_artifact = read_file(artifact);
    write(artifact, original_artifact + " ");
    expect(capture([&] { return run_check(options); }).first != 0, "cached check hid corrupt output");
    expect(!assess_index_freshness(options, true).current, "cached freshness hid corrupt output");
    auto plain = options; plain.validation_generation.reset();
    expect(capture([&] { return run_check(options); }).second == capture([&] { return run_check(plain); }).second,
           "corruption diagnostics differ");
    const auto manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    const auto manifest_bytes = read_file(manifest_path);
    auto forged = parse_json_text(manifest_bytes, "manifest");
    for (auto& row : forged["generated_file_integrity"]["files"])
        if (row["path"] == (kGeneratedDir / "documents.jsonl").generic_string()) {
            row["bytes"] = original_artifact.size() + 1;
            row["content_hash"] = fnv1a64(original_artifact + " ");
        }
    write(manifest_path, forged.dump());
    expect(capture([&] { return run_check(options); }).first != 0 &&
           !assess_index_freshness(options, true).current, "self-consistent forged output bypassed expected bytes");
    write(manifest_path, manifest_bytes);
    write(artifact, original_artifact);
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto base = fs::temp_directory_path() / ("validation-generation-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { test(base, argv[1]); fs::remove_all(base); std::cout << "validation generation boundaries passed\n"; return 0; }
    catch (const std::exception& error) { std::error_code ignored; fs::remove_all(base, ignored);
        std::cerr << error.what() << '\n'; return 1; }
}
