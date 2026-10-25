#include "context_internal.h"
#include "profile_policy.h"

namespace {
using namespace shin::context;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exercise(const fs::path& directory) {
    const auto root = directory / "root";
    const auto registry = directory / "profiles";
    fs::create_directories(root);
    fs::create_directories(registry);
    { std::ofstream file(root / "README.md"); file << "# Policy fixture\n"; }
    const Json config = {{"id", "fixture"}, {"sourceLabel", "Fixture"},
                         {"rootFiles", {"README.md"}}, {"bootstrapOnly", true}};
    { std::ofstream file(registry / "fixture.json"); file << config.dump(); }
    const auto policy = inspect_profile_policy(root, registry, "fixture");
    expect(policy.at("ok") == true, "typed inspection failed");
    const auto hash = policy.at("revision").at("profile_config_hash").get<std::string>();
    expect(hash == make_profile("fixture", registry).config_hash, "profile identity drifted");
    expect(policy == inspect_profile_policy(root, registry, "fixture", hash), "inspection is not deterministic");
    expect(policy.at("fields").at("sourceFactMaxFiles").at("value") == Profile{}.source_fact_max_files,
           "defaults differ from Core");
    expect(policy.at("fields").at("agentStartPaths").at("origin") == "derived-root-files",
           "derived provenance lost");
    RuntimeOptions options;
    options.root = root;
    options.profile_dir = registry;
    options.profile = make_profile("fixture", registry);
    std::ostringstream output;
    auto* saved = std::cout.rdbuf(output.rdbuf());
    int code = -1;
    try {
        code = run_profile_policy(options, {"--expect-profile-hash", hash});
    } catch (...) {
        std::cout.rdbuf(saved);
        throw;
    }
    std::cout.rdbuf(saved);
    expect(code == 0 && Json::parse(output.str()) == policy, "CLI and embedded Core disagree");
    { std::ofstream file(registry / "fixture.json", std::ios::app); file << '\n'; }
    bool conflict = false;
    try { (void)inspect_profile_policy(root, registry, "fixture", hash); }
    catch (const CliError& error) { conflict = error.code() == "profile-policy-revision-conflict"; }
    expect(conflict, "stale profile revision was accepted");
    expect(!fs::exists(root / kGeneratedDir), "inspection published an index");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto path = std::filesystem::path(argv[1]) /
        ("policy-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int code = 0;
    try { exercise(path); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; code = 1; }
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return code;
}
