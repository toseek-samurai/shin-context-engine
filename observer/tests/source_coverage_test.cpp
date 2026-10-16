#include "shin/observer/local_query.h"
#include "shin/observer/observer.h"
#include "shin/observer/local_mcp_scope.h"
#include "shin/observer_protocol.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace shin::observer;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    fs::path base, root;
    Config config;
    explicit Fixture(const fs::path& profiles) {
        std::string pattern = (fs::temp_directory_path() / "shin-observation-XXXXXX").string();
        check(mkdtemp(pattern.data()) != nullptr, "mkdtemp");
        base = fs::canonical(pattern); root = base / "source";
        fs::create_directories(root / "docs");
        config.root = root; config.profile_dir = fs::canonical(profiles); config.profile = "generic-source";
        config.out_dir = base / "absent-index";
        std::ofstream(root / "README.md") << "# Source\n";
        std::ofstream(root / "limits.js") << "export function limit() { return 7; }\n";
        std::ofstream(root / "docs/guide.md") << "# Guide\n";
    }
    ~Fixture() noexcept {
        try {
            chmod((root / "docs").c_str(), 0700); chmod((root / "limits.js").c_str(), 0600);
            std::error_code ec; fs::remove_all(base, ec);
        } catch (...) { std::terminate(); }
    }
    Json sample() const {
        const auto value = sample_worktree(config, directory_identity(root), true);
        shin::observer_protocol::verify_observation(value);
        return value;
    }
};
}
namespace {
int coverage_projection() try {
    using namespace shin::observer;
    const auto expect = [](bool value) { if (!value) throw std::runtime_error("source coverage projection"); };
    expect(source_coverage_summary(Json::object()).is_null());
    Json manifest{{"sources", Json::array()}, {"source_fact_sources", Json::array()}};
    for (int i = 0; i < 12; ++i) {
        const Json source{{"path", "src/" + std::to_string(i) + ".cpp"}, {"quarantine_reason", "SENSITIVE_VALUE"}};
        manifest["sources"].push_back(source);
        manifest["source_fact_sources"].push_back(source);
    }
    auto result = source_coverage_summary(manifest);
    expect(result.at("quarantined_files") == 12 && result.at("files").size() == 8 && result.at("omitted_files") == 4);
    expect(result.dump().find("SENSITIVE_VALUE") == std::string::npos);
    for (auto& source : manifest["sources"]) source["path"] = std::string(4000, '\n') + source.at("path").get<std::string>();
    manifest["source_fact_sources"] = Json::array();
    result = source_coverage_summary(manifest);
    expect(result.at("files").empty() && result.at("omitted_files") == 12 && result.dump().size() < 4096);
    return 0;
} catch (...) { return 1; }

}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "profiles required");
        check(coverage_projection() == 0, "source coverage projection");
        Fixture f(argv[1]);
        const auto initial = f.sample();
        check(initial.at("status") == "unpublished", initial.dump().c_str());
        if (geteuid() == 0) std::cout << "permission checks skipped for privileged caller\n";
        if (geteuid() != 0) for (const auto& path : {f.root / "limits.js", f.root / "docs"}) {
            check(chmod(path.c_str(), 0000) == 0, "deny fixture read");
            const auto result = f.sample();
            check(chmod(path.c_str(), path == f.root / "docs" ? 0700 : 0600) == 0, "restore fixture read");
            check(result.at("status") == "error" && result.at("reason_code") == "access-denied", "permission misclassified as race");
            const auto& diagnostic = result.at("diagnostic");
            check(diagnostic.at("category") == "access-denied" && diagnostic.dump().find(f.base.string()) == std::string::npos,
                  "source path disclosed in diagnostic");
            const auto error = Error("access-denied", "Source observation failed", diagnostic);
            check(error_json(error).at("diagnostic") == diagnostic, "error lost diagnostic");
            const auto help = local_mcp_recovery(error.code, error.diagnostic);
            check(help.at("kind") == "environment-action" && help.at("diagnostic") == diagnostic && !help.contains("retry_after_ms"),
                  "permanent failure got retry or lost cause");
            check(f.sample().at("status") == "unpublished", "recovery after permission restore failed");
        }
        std::ofstream(f.root / "limits.js") << std::string(1048577, 'x');
        const auto incomplete = f.sample();
        check(incomplete.at("reason_code") == "incomplete-source-observation" && !incomplete.contains("diagnostic"), "source cap changed");
        std::cout << "observation diagnostics passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
