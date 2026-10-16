#include "shin/observer/local_mcp_scope.h"
#include <chrono>
#include <iostream>
#include <future>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace shin::observer;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void refused(const char* code, F action) {
    try { action(); }
    catch (const Error& error) { check(error.code == code, "unexpected refusal"); return; }
    throw std::runtime_error("unexpected admission");
}
struct Fixture {
    fs::path base = fs::canonical(fs::temp_directory_path()) / ("shin-mcp-scope-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path home = base / "home";
    Fixture() { ensure_private_directory(base, true); ensure_private_directory(home, true); ensure_private_directory(home / "projects", true); }
    ~Fixture() { std::error_code ignored; fs::remove_all(base, ignored); }
    fs::path root(const fs::path& parent) {
        ensure_private_directory(base / parent, true);
        const auto path = base / parent / "same-name";
        ensure_private_directory(path, true);
        ensure_private_directory(workspace(path), true);
        return path;
    }
    fs::path workspace(const fs::path& path) const { return home / "projects" / digest(path.string()); }
    Json binding(const fs::path& path) const {
        return {{"root", path.string()}, {"workspace_identity", directory_identity(workspace(path), true)}};
    }
};
}
int main() {
    try {
#ifndef _WIN32
        ::umask(0077);
#endif
        for (const auto* code : {"project-updating", "refresh-timeout", "refresh-suspended", "service-busy"}) {
            const auto hint = local_mcp_recovery(code);
            check(hint.at("kind") == "transient" && hint.at("max_attempts_per_checkpoint") == 1 &&
                hint.at("same_project_required") == true && hint.at("fresh_snapshot_required") == true,
                "transient retry must remain bounded and project/snapshot scoped");
        }
        for (const auto* code : {"file-changed-during-verification", "query-source-changed",
                "source-read-hash-mismatch", "refresh-superseded", "query-snapshot-changed"}) {
            const auto hint = local_mcp_recovery(code);
            check(hint.at("kind") == "transient" && hint.at("max_attempts_per_checkpoint") == 1 &&
                hint.at("next_tool") == "shin_query", "known source races must have one bounded retry");
        }
        const auto review = local_mcp_recovery("route-revision-conflict");
        check(review.at("kind") == "operator-review" && review.at("approval_required") == true &&
            review.at("same_project_required") == true && review.at("fresh_snapshot_required") == true &&
            !review.contains("retry_after_ms") && !review.contains("next_tool"),
            "policy review must not imply retry or MCP mutation authority");
        check(review.at("message").get<std::string>().find("profile-review") != std::string::npos,
            "route review must retain the established owner profile-review flow");
        for (const auto* code : {"binding-identity-changed", "project-detached", "mcp-project-changed", "source-binding-changed", "refresh-engine-changed"}) {
            const auto hint = local_mcp_recovery(code);
            check(hint.at("kind") == "operator-review" && !hint.contains("retry_after_ms") &&
                !hint.contains("next_tool"), "binding and lifecycle changes need review, not retries");
        }
        for (const auto* code : {"storage-full", "storage-read-only", "access-denied", "source-io-error", "resource-exhausted"}) {
            const auto hint = local_mcp_recovery(code);
            check(hint.at("kind") == "environment-action" && !hint.contains("retry_after_ms") &&
                !hint.contains("next_tool"), "environment conditions must not get generic retries");
        }
        const auto denied_hint = local_mcp_recovery("mcp-project-denied");
        check(denied_hint.at("kind") == "request-correction" &&
            denied_hint.at("next_action") == "discover-allowed-project-catalog", "denied selector guidance");
        const auto mismatch = local_mcp_recovery("mcp-snapshot-scope-mismatch");
        check(mismatch.at("kind") == "request-correction" && mismatch.at("same_project_required") == true &&
            mismatch.at("fresh_snapshot_required") == true, "snapshot mismatch must stay same-project scoped");
        const Json observation_diagnostic{{"schema_version", "shin.source-observation-diagnostic.v1"},
            {"stage", "source-inventory"}, {"operation", "read-source-content"}, {"category", "io-error"}};
        check(local_mcp_recovery("source-io-error", observation_diagnostic).at("diagnostic") == observation_diagnostic,
            "I/O diagnostic lost in guidance");
        auto unsafe_observation = observation_diagnostic;
        unsafe_observation["path"] = "/private/source";
        check(!local_mcp_recovery("source-io-error", unsafe_observation).contains("diagnostic"), "unsafe diagnostic admitted");
        const auto source_failure = local_mcp_recovery("source-observation-failed");
        const auto unknown = local_mcp_recovery("some-internal-code");
        for (const auto& hint : {source_failure, unknown})
            check(hint.at("kind") == "maintainer-diagnosis" && hint.at("cause_status") == "unconfirmed" &&
                hint.at("responsible_party") == "shin-maintainers" && !hint.contains("retry_after_ms") &&
                !hint.contains("next_tool"), "unclassified failures must not invent a cause or retry");
        const auto validation = local_mcp_recovery("refresh-validation-failed");
        check(validation.at("kind") == "project-check" && validation.at("message").get<std::string>().find(
            "does not establish that a file was deleted") != std::string::npos,
            "project check guidance must not infer deleted paths");
        Json malformed{{"schema_version", "shin.validation-diagnostic.v1"}, {"message", "secret source body"},
            {"absolute_path", "/Users/private/source.cpp"}, {"gates", Json::array({
                Json{{"id", "safe-gate"}, {"message", "secret source body"}, {"cases", Json::array({
                    Json{{"id", "safe-case"}, {"reasons", Json::array({"missing-required-path"})},
                        {"expected_paths", Json::array({"../../private", "/Users/private/source.cpp", "docs/guide.md"})}}
                })}}
            })}};
        const auto sanitized = local_mcp_recovery("project-check-failed", malformed);
        const auto rendered = sanitized.dump();
        check(sanitized.contains("diagnostic") && sanitized.at("failure_reason") == "missing-required-path" &&
            rendered.find("secret source body") == std::string::npos &&
            rendered.find("/Users/private") == std::string::npos &&
            rendered.find("../../private") == std::string::npos &&
            rendered.find("docs/guide.md") != std::string::npos,
            "project diagnostic must retain only bounded safe identifiers and relative paths");
        for (const auto* code : {"service-unavailable", "service-version-mismatch", "service-disconnected"}) {
            const auto hint = local_mcp_recovery(code);
            check(hint.at("kind") == "environment-action" && hint.at("responsible_party") == "operator" &&
                !hint.contains("retry_after_ms"), "service setup must not be diagnosed as an engine defect");
        }
        check(local_mcp_recovery("service-request-cancelled").at("kind") == "cancelled",
            "intentional cancellation must not request repair");
        check(local_mcp_recovery("source-snapshot-mismatch").at("kind") == "request-correction",
            "a stale follow-up needs a new same-project snapshot");
        check(local_mcp_recovery("refresh-source-budget").at("approval_required") == true,
            "resource limits must not grant automatic policy changes");
        for (const auto* code : {"project-updating", "route-revision-conflict", "refresh-validation-failed", "unknown"})
            check(local_mcp_recovery(code, malformed).dump().size() < 8192, "guidance exceeds its bounded surface");
        const auto help = local_mcp_help();
        check(help.at("ok") == true && help.at("schema_version") == "shin.mcp-help.v1" &&
            help.at("scope") == "static-guidance" && help.at("readiness_assessed") == false &&
            help.at("automatic_upload") == false && !help.contains("projects"), "help must be static and unscoped");
        check(local_mcp_help("source-observation-failed").at("recovery").at("cause_status") == "unconfirmed",
            "help must use the recovery classifier");
        refused("invalid-arguments", [&] { (void)local_mcp_help("bad code with secrets"); });
        Fixture f;
        const auto a = f.root("a"), b = f.root(fs::path(u8"\u043f\u0440\u043e\u0435\u043a\u0442")), denied = f.root("denied");
        LocalMcpScope scopes(f.home, {a, b});
        const auto catalog = scopes.catalog();
        check(catalog.at("projects").size() == 2 && catalog.at("readiness_assessed") == false, "catalog scope");
        const auto pa = catalog.at("projects")[0].at("project").get<std::string>();
        const auto pb = catalog.at("projects")[1].at("project").get<std::string>();
        check(pa != pb && catalog.at("projects")[0].at("name") == catalog.at("projects")[1].at("name"), "labels conflated identities");
        const LocalMcpScope reopened(f.home, {b, a});
        check(reopened.select(pa).root == a && reopened.select(pb).root == b, "selectors changed on reconnect/order");
        refused("mcp-project-denied", [&] { (void)scopes.select(denied.string()); });
        refused("invalid-arguments", [&] { LocalMcpScope empty(f.home, {fs::path(), a}); });
        refused("invalid-arguments", [&] { LocalMcpScope duplicate(f.home, {a, a / "."}); });
        refused("invalid-arguments", [&] { LocalMcpScope excessive(f.home, std::vector<fs::path>(17, a)); });
        // Even identical native snapshots must not turn a foreign receipt into
        // a valid continuation for another project.
        const auto snapshot = std::string("fnv1a64:0123456789abcdef");
        const auto sa = scopes.snapshot(pa, snapshot), sb = scopes.snapshot(pb, snapshot);
        check(sa != sb && scopes.native_snapshot(pa, sa) == snapshot, "snapshot scoping");
        refused("mcp-snapshot-scope-mismatch", [&] { (void)scopes.native_snapshot(pb, sa); });
        refused("mcp-snapshot-scope-mismatch", [&] { (void)scopes.native_snapshot(pa, snapshot); });
        auto left = std::async(std::launch::async, [&] { for (int i=0;i<20;++i) check(scopes.select(pa).root == a, "parallel A changed"); });
        auto right = std::async(std::launch::async, [&] { for (int i=0;i<20;++i) check(scopes.select(pb).root == b, "parallel B changed"); });
        left.get(); right.get();
        const auto missing = f.base / "missing";
        const LocalMcpScope partial(f.home, {missing, b});
        const auto missing_handle = partial.catalog().at("projects")[0].at("project").get<std::string>();
        check(!partial.catalog().at("projects")[0].at("binding_active").get<bool>() && partial.select(pb).root == b,
              "unavailable startup member disabled neighbor");
        ensure_private_directory(missing, true);
        refused("project-unavailable", [&] { partial.verify(missing_handle); });
        set_local_project_lifecycle(f.workspace(a), f.binding(a), "detached");
        refused("project-detached", [&] { scopes.verify(pa); });
        const LocalMcpScope detached_start(f.home, {a, b});
        check(!detached_start.catalog().at("projects")[0].at("binding_active").get<bool>() && detached_start.select(pb).root == b, "detached startup member disabled neighbor");
        check(!scopes.catalog().at("projects")[0].at("binding_active").get<bool>() && scopes.select(pb).root == b, "detach affected neighbor");
        set_local_project_lifecycle(f.workspace(a), f.binding(a), "active");
        refused("mcp-project-changed", [&] { scopes.verify(pa); });
        const LocalMcpScope reattached(f.home, {a, b});
        const auto fresh = reattached.catalog().at("projects")[0].at("project").get<std::string>();
        check(fresh != pa, "reattach reused revoked selector");
        refused("mcp-project-denied", [&] { (void)reattached.select(pa); });
        // OS path replacement cannot be adopted by an existing connection.
        fs::rename(b, b.parent_path() / "previous");
        ensure_private_directory(b, true);
        refused("binding-identity-changed", [&] { scopes.verify(pb); });
        std::cout << "MCP explicit scope, parallel isolation, snapshots, lifecycle and replacement passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
