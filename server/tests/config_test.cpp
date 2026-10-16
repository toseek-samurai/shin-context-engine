#include "shin/server/config.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <functional>
#include <iostream>

namespace {
using namespace shin::server;
namespace fs = std::filesystem;

void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

void rejects(const char* code, const std::function<void()>& action) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.code == code, "unexpected error code");
        return;
    }
    throw std::runtime_error("invalid input was accepted");
}

class Fixture {
public:
    fs::path root;
    fs::path config_path;
    fs::path recovery_path;
    fs::path connection_path;
    fs::path preparations_path;
    std::string token = std::string(64, 'a');
    Json value;

    Fixture() {
        auto pattern = (fs::temp_directory_path() / "shin-server-config-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error("temporary directory failed");
        root = fs::canonical(pattern);
        fs::create_directory(root / "allowed");
        fs::create_directory(root / "allowed-neighbor");
        fs::create_directory(root / "allowed" / "project");
        fs::create_directory(root / "profiles");
        preparations_path = root / "preparations";
        fs::create_directory(preparations_path);
        if (::chmod(preparations_path.c_str(), 0700) != 0) throw std::runtime_error("chmod failed");
        config_path = root / "config.json";
        recovery_path = root / "recovery.json";
        connection_path = root / "connection";
        write(connection_path, "host=127.0.0.1 dbname=fixture user=fixture");
        value = {
            {"schema_version", "shin.server-config.v10"},
            {"source_host_id", "local-test"},
            {"listen", {{"host", "127.0.0.1"}, {"port", 19125}}},
            {"database", {{"connection_file", connection_path.string()}, {"expected_name", "fixture"},
                {"server_id", "0123456789abcdef0123456789abcdef"}, {"runtime_role", "fixture"}}},
            {"allowed_roots", Json::array({(root / "allowed").string()})},
            {"principals", Json::array({
                {{"id", "owner"}, {"role", "admin"}, {"token_sha256", digest_hex(sha256(token))}, {"project_ids", Json::array()}},
                {{"id", "reader"}, {"role", "reader"}, {"token_sha256", digest_hex(sha256(std::string(64, 'c')))}, {"project_ids", Json::array({"visible"})}}
            })},
            {"retrieval", {
                {"routes", Json::array({route("visible-route")})},
                {"max_payload_bytes", 512 * 1024 * 1024},
                {"max_generation_bytes", 128 * 1024 * 1024},
                {"max_file_bytes", 64 * 1024 * 1024},
                {"max_generations", 8}
            }},
            {"knowledge", {
                {"enabled", false},
                {"principal_id", ""},
                {"route_ids", Json::array()},
                {"interval_seconds", 60},
                {"max_age_seconds", 180},
                {"prepare_on_drift", false}
            }},
            {"journal_writer", {
                {"enabled", false},
                {"principal_id", ""},
                {"interval_ms", 1000},
                {"max_retry_ms", 30000},
                {"flush_on_shutdown", true}
            }},
            {"preparations", preparations(preparations_path)},
            {"observability", {{"max_retained_requests", 128}, {"max_query_bytes", 4096},
                {"capture_query_projects", Json::array()}}},
            {"workers", 4}, {"queue_limit", 64}
        };
        save();
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    void write(const fs::path& path, const std::string& text) {
        std::ofstream stream(path);
        stream << text;
        stream.close();
        if (::chmod(path.c_str(), 0600) != 0) throw std::runtime_error("chmod failed");
    }
    void save() { write(config_path, value.dump()); }
    Json route(const std::string& id) const {
        return {
            {"id", id},
            {"project_id", "visible"},
            {"workspace_id", "primary"},
            {"root", (root / "allowed" / "project").string()},
            {"profile", "server"},
            {"profile_dir", (root / "profiles").string()}
        };
    }
    Json preparations(const fs::path& state_dir) const {
        return {
            {"enabled", false},
            {"state_dir", state_dir.string()},
            {"max_retained_jobs", 8},
            {"lease_seconds", 30}
        };
    }
    Json project() const {
        return {{"id", "visible"}, {"display_name", "Visible project"}};
    }
    Json workspace(const fs::path& path) const {
        return {{"id", "primary"}, {"display_name", "Primary workspace"}, {"root", path.string()}};
    }
};

} // namespace

int main() {
    try {
        Fixture fixture;
        const auto config = load_config(fixture.config_path);
        check(!config.credentials.at(0).principal.receipt_producer &&
              !config.credentials.at(1).principal.receipt_producer, "receipt permission must default to false even for admin");
        const ObservabilityConfig defaults;
        check(defaults.max_retained_requests == 128 && defaults.max_query_bytes == 4096 &&
                  defaults.capture_query_projects.empty(), "observability defaults changed");
        check(config.observability.max_retained_requests == 128 && config.observability.max_query_bytes == 4096 &&
                  config.observability.capture_query_projects.empty(), "observability configuration was not loaded");
        check(!config.knowledge.enabled && config.knowledge.principal_id.empty() && config.knowledge.route_ids.empty() &&
                  config.knowledge.interval_seconds == 60 && config.knowledge.max_age_seconds == 180 &&
                  !config.knowledge.prepare_on_drift, "knowledge configuration defaults were not loaded");
        check(!config.journal_writer.enabled && config.journal_writer.principal_id.empty() &&
                  config.journal_writer.interval_ms == 1000 && config.journal_writer.max_retry_ms == 30000 &&
                  config.journal_writer.flush_on_shutdown,
              "journal writer configuration defaults were not loaded");
        const auto baseline = fixture.value;
        const auto reject_config = [&](const Json& value) {
            fixture.value = value;
            fixture.save();
            rejects("invalid-config", [&] { load_config(fixture.config_path); });
        };
        auto missing_observability = baseline;
        for (const auto& invalid_permission : Json::array({nullptr, 1, "true", Json::array()})) {
            auto invalid = baseline;
            invalid["principals"][0]["receipt_producer"] = invalid_permission;
            reject_config(invalid);
        }
        fixture.value = baseline;
        fixture.value["principals"][1]["receipt_producer"] = true;
        fixture.save();
        const auto producer = load_config(fixture.config_path);
        check(producer.credentials.at(1).principal.receipt_producer &&
              !producer.credentials.at(1).principal.admin, "receipt permission must be independent of admin");
        auto oidc_config = baseline;
        oidc_config["oidc"] = {{"issuer", "https://identity.example.test/realm"},
                               {"audience", "shin-server"}, {"client_id", "shin-ce"}, {"ca_file", ""}};
        fixture.value = oidc_config;
        fixture.save();
        const auto oidc_loaded = load_config(fixture.config_path);
        check(oidc_loaded.oidc && oidc_loaded.oidc->trust.client_id == "shin-ce", "OIDC configuration lost");
        for (const auto* field : {"issuer", "audience", "client_id", "ca_file"}) {
            auto invalid = oidc_config;
            invalid["oidc"].erase(field);
            reject_config(invalid);
        }
        for (const auto& override_value : Json::array({Json{{"issuer", "http://idp"}},
                Json{{"audience", "shin-ce"}}, Json{{"ca_file", "relative.pem"}}, Json{{"extra", true}}})) {
            auto invalid = oidc_config;
            invalid["oidc"].update(override_value);
            reject_config(invalid);
        }
        auto old_schema = baseline;
        old_schema["schema_version"] = "shin.server-config.v9";
        reject_config(old_schema);
        missing_observability.erase("observability");
        reject_config(missing_observability);
        for (const auto& field : {"max_retained_requests", "max_query_bytes", "capture_query_projects"}) {
            auto missing = baseline;
            missing["observability"].erase(field);
            reject_config(missing);
        }
        auto extra = baseline;
        extra["observability"]["unknown"] = true;
        reject_config(extra);
        auto missing_knowledge = baseline;
        missing_knowledge.erase("knowledge");
        reject_config(missing_knowledge);
        for (const auto& field : {"enabled", "principal_id", "route_ids", "interval_seconds", "max_age_seconds", "prepare_on_drift"}) {
            auto missing = baseline;
            missing["knowledge"].erase(field);
            reject_config(missing);
        }
        auto extra_knowledge = baseline;
        extra_knowledge["knowledge"]["unknown"] = true;
        reject_config(extra_knowledge);
        for (const auto& invalid_object : Json::array({nullptr, false, 0, "knowledge", Json::array()})) {
            auto invalid = baseline;
            invalid["knowledge"] = invalid_object;
            reject_config(invalid);
        }
        auto missing_journal_writer = baseline;
        missing_journal_writer.erase("journal_writer");
        reject_config(missing_journal_writer);
        for (const auto& field : {"enabled", "principal_id", "interval_ms", "max_retry_ms", "flush_on_shutdown"}) {
            auto missing = baseline;
            missing["journal_writer"].erase(field);
            reject_config(missing);
        }
        auto extra_journal_writer = baseline;
        extra_journal_writer["journal_writer"]["unknown"] = true;
        reject_config(extra_journal_writer);
        for (const auto& invalid_object : Json::array({nullptr, false, 0, "journal_writer", Json::array()})) {
            auto invalid = baseline;
            invalid["journal_writer"] = invalid_object;
            reject_config(invalid);
        }
        const auto journal_writer_config = [](bool enabled, const std::string& principal_id,
                                              std::uint64_t interval_ms = 1000,
                                              std::uint64_t max_retry_ms = 30000,
                                              bool flush_on_shutdown = true) {
            return Json{{"enabled", enabled}, {"principal_id", principal_id}, {"interval_ms", interval_ms},
                        {"max_retry_ms", max_retry_ms}, {"flush_on_shutdown", flush_on_shutdown}};
        };
        auto enabled_journal_writer = baseline;
        enabled_journal_writer["journal_writer"] = journal_writer_config(true, "owner");
        fixture.value = enabled_journal_writer;
        fixture.save();
        const auto enabled_writer_config = load_config(fixture.config_path);
        check(enabled_writer_config.journal_writer.enabled && enabled_writer_config.journal_writer.principal_id == "owner" &&
                  enabled_writer_config.journal_writer.interval_ms == 1000 &&
                  enabled_writer_config.journal_writer.max_retry_ms == 30000 &&
                  enabled_writer_config.journal_writer.flush_on_shutdown,
              "enabled journal writer configuration was not loaded");
        for (const auto& principal : {"", "missing-admin", "reader"}) {
            auto invalid = enabled_journal_writer;
            invalid["journal_writer"] = journal_writer_config(true, principal);
            reject_config(invalid);
        }
        for (const auto& disabled_writer : Json::array({
                 journal_writer_config(false, "owner"),
                 journal_writer_config(false, "reader")
             })) {
            auto invalid = baseline;
            invalid["journal_writer"] = disabled_writer;
            reject_config(invalid);
        }
        for (const auto& field : {"enabled", "flush_on_shutdown"}) {
            for (const auto& invalid_boolean : Json::array({0, 1, "true", nullptr, Json::array(), Json::object()})) {
                auto invalid = baseline;
                invalid["journal_writer"][field] = invalid_boolean;
                reject_config(invalid);
            }
        }
        for (const auto& invalid_principal : Json::array({nullptr, false, 0, Json::array(), Json::object(),
                                                           std::string(64, 'a'), "Upper", "two words"})) {
            auto invalid = enabled_journal_writer;
            invalid["journal_writer"]["principal_id"] = invalid_principal;
            reject_config(invalid);
        }
        for (const auto& field : {"interval_ms", "max_retry_ms"}) {
            for (const auto& invalid_number : Json::array({-1, -0.5, 1.0, true, "1000", nullptr,
                                                             Json::array(), Json::object(), UINT64_MAX})) {
                auto invalid = enabled_journal_writer;
                invalid["journal_writer"][field] = invalid_number;
                reject_config(invalid);
            }
        }
        for (const auto& invalid_policy : Json::array({
                 journal_writer_config(true, "owner", 99, 30000),
                 journal_writer_config(true, "owner", 60001, 60001),
                 journal_writer_config(true, "owner", 100, 99),
                 journal_writer_config(true, "owner", 100, 300001)
             })) {
            auto invalid = baseline;
            invalid["journal_writer"] = invalid_policy;
            reject_config(invalid);
        }
        for (const auto& policy : Json::array({
                 journal_writer_config(true, "owner", 100, 100, false),
                 journal_writer_config(true, "owner", 60000, 60000),
                 journal_writer_config(true, "owner", 100, 300000)
             })) {
            fixture.value = baseline;
            fixture.value["journal_writer"] = policy;
            fixture.save();
            const auto loaded = load_config(fixture.config_path).journal_writer;
            check(loaded.enabled && loaded.principal_id == "owner" &&
                      loaded.interval_ms == policy["interval_ms"].get<int>() &&
                      loaded.max_retry_ms == policy["max_retry_ms"].get<int>() &&
                      loaded.flush_on_shutdown == policy["flush_on_shutdown"].get<bool>(),
                  "journal writer numeric boundary rejected");
        }
        auto zero_retention_writer = enabled_journal_writer;
        zero_retention_writer["observability"]["max_retained_requests"] = 0;
        reject_config(zero_retention_writer);
        fixture.value = baseline;
        fixture.save();
        const auto knowledge_config = [](bool enabled, const std::string& principal_id, const Json& route_ids,
                                         std::uint64_t interval_seconds = 60, std::uint64_t max_age_seconds = 180,
                                         bool prepare_on_drift = false) {
            return Json{{"enabled", enabled}, {"principal_id", principal_id}, {"route_ids", route_ids},
                        {"interval_seconds", interval_seconds}, {"max_age_seconds", max_age_seconds},
                        {"prepare_on_drift", prepare_on_drift}};
        };
        const auto one_route = Json::array({"visible-route"});
        auto enabled_knowledge = baseline;
        enabled_knowledge["knowledge"] = knowledge_config(true, "owner", one_route);
        fixture.value = enabled_knowledge;
        fixture.save();
        const auto enabled_config = load_config(fixture.config_path);
        check(enabled_config.knowledge.enabled && enabled_config.knowledge.principal_id == "owner" &&
                  enabled_config.knowledge.route_ids == std::vector<std::string>{"visible-route"},
              "enabled knowledge configuration was not loaded");
        for (const auto& principal : {"", "missing-admin", "reader"}) {
            auto invalid = enabled_knowledge;
            invalid["knowledge"] = knowledge_config(true, principal, one_route);
            reject_config(invalid);
        }
        for (const auto& routes : Json::array({Json::array(), Json::array({"missing-route"}),
                                                Json::array({"visible-route", "visible-route"}),
                                                Json::array({1}), Json::array({nullptr}), "visible-route"})) {
            auto invalid = enabled_knowledge;
            invalid["knowledge"] = knowledge_config(true, "owner", routes);
            reject_config(invalid);
        }
        auto nine_routes = baseline;
        nine_routes["retrieval"]["routes"] = Json::array();
        auto nine_route_ids = Json::array();
        for (int index = 0; index < 9; ++index) {
            const auto id = "route-" + std::to_string(index);
            nine_routes["retrieval"]["routes"].push_back(fixture.route(id));
            nine_route_ids.push_back(id);
        }
        nine_routes["knowledge"] = knowledge_config(true, "owner", nine_route_ids);
        reject_config(nine_routes);
        for (const auto& boundary : Json::array({{{"interval_seconds", 0}}, {{"interval_seconds", 3601}},
                                                  {{"max_age_seconds", 0}}, {{"max_age_seconds", 86401}},
                                                  {{"max_age_seconds", 59}}})) {
            auto invalid = enabled_knowledge;
            invalid["knowledge"].update(boundary);
            reject_config(invalid);
        }
        for (const auto& field : {"interval_seconds", "max_age_seconds"}) {
            for (const auto& invalid_number : Json::array({-1, -0.5, 1.0, true, "60", nullptr,
                                                             Json::array(), Json::object(), UINT64_MAX})) {
                auto invalid = enabled_knowledge;
                invalid["knowledge"][field] = invalid_number;
                reject_config(invalid);
            }
        }
        auto minimum_intervals = enabled_knowledge;
        minimum_intervals["knowledge"] = knowledge_config(true, "owner", one_route, 1, 1);
        fixture.value = minimum_intervals;
        fixture.save();
        const auto minimum_config = load_config(fixture.config_path);
        check(minimum_config.knowledge.interval_seconds == 1 &&
                  minimum_config.knowledge.max_age_seconds == 1,
              "knowledge minimum interval and age were rejected");
        for (const auto& disabled_change : Json::array({
                 knowledge_config(false, "owner", Json::array()),
                 knowledge_config(false, "", one_route),
                 knowledge_config(false, "", Json::array(), 60, 180, true)
             })) {
            auto invalid = baseline;
            invalid["knowledge"] = disabled_change;
            reject_config(invalid);
        }
        auto preparation_required = enabled_knowledge;
        preparation_required["knowledge"] = knowledge_config(true, "owner", one_route, 1, 1, true);
        reject_config(preparation_required);
        preparation_required["preparations"] = fixture.preparations(fixture.preparations_path);
        preparation_required["preparations"]["enabled"] = true;
        fixture.value = preparation_required;
        fixture.save();
        check(load_config(fixture.config_path).knowledge.prepare_on_drift,
              "knowledge preparation policy rejected with enabled preparations");
        auto two_routes = baseline;
        two_routes["retrieval"]["routes"] = Json::array({fixture.route("route-a"), fixture.route("route-b")});
        two_routes["knowledge"] = knowledge_config(true, "owner", Json::array({"route-a", "route-b"}));
        two_routes["retrieval"]["max_generations"] = 1;
        reject_config(two_routes);
        two_routes["retrieval"]["max_generations"] = 8;
        two_routes["retrieval"]["max_payload_bytes"] = 3;
        two_routes["retrieval"]["max_generation_bytes"] = 2;
        two_routes["retrieval"]["max_file_bytes"] = 2;
        reject_config(two_routes);
        auto eight_routes = baseline;
        eight_routes["retrieval"]["routes"] = Json::array();
        auto eight_route_ids = Json::array();
        for (int index = 0; index < 8; ++index) {
            const auto id = "route-" + std::to_string(index);
            eight_routes["retrieval"]["routes"].push_back(fixture.route(id));
            eight_route_ids.push_back(id);
        }
        eight_routes["retrieval"]["max_payload_bytes"] = 8;
        eight_routes["retrieval"]["max_generation_bytes"] = 1;
        eight_routes["retrieval"]["max_file_bytes"] = 1;
        eight_routes["retrieval"]["max_generations"] = 8;
        eight_routes["knowledge"] = knowledge_config(true, "owner", eight_route_ids, 3600, 86400);
        fixture.value = eight_routes;
        fixture.save();
        check(load_config(fixture.config_path).knowledge.route_ids.size() == 8,
              "knowledge route-count boundary rejected");
        fixture.value = baseline;
        fixture.save();
        for (const auto& invalid_object : Json::array({nullptr, false, 0, "observability", Json::array()})) {
            auto invalid = baseline;
            invalid["observability"] = invalid_object;
            reject_config(invalid);
        }
        for (const auto& field : {"max_retained_requests", "max_query_bytes"}) {
            for (const auto& invalid_number : Json::array({-1, -0.5, 1.0, true, "1", nullptr,
                                                         Json::array(), Json::object(), UINT64_MAX})) {
                auto invalid = baseline;
                invalid["observability"][field] = invalid_number;
                reject_config(invalid);
            }
        }
        for (const auto& boundary : Json::array({
                 {{"max_retained_requests", 257}}, {{"max_query_bytes", 0}}, {{"max_query_bytes", 8193}}})) {
            auto invalid = baseline;
            invalid["observability"].update(boundary);
            reject_config(invalid);
        }
        for (const auto retained : {0, 1, 128, 256}) {
            for (const auto bytes : {1, 4096, 8192}) {
                fixture.value = baseline;
                fixture.value["observability"]["max_retained_requests"] = retained;
                fixture.value["observability"]["max_query_bytes"] = bytes;
                fixture.save();
                const auto loaded = load_config(fixture.config_path).observability;
                check(loaded.max_retained_requests == static_cast<std::size_t>(retained) &&
                          loaded.max_query_bytes == static_cast<std::size_t>(bytes) &&
                          loaded.capture_query_projects.empty(), "observability numeric boundary rejected");
            }
        }
        for (const auto& invalid_projects : Json::array({nullptr, false, 1, "visible", Json::object(),
                 Json::array({1}), Json::array({nullptr}), Json::array({true}), Json::array({Json::object()}),
                 Json::array({Json::array()}), Json::array({""}), Json::array({"*"}), Json::array({"visible-*"}),
                 Json::array({"Upper"}), Json::array({"1project"}), Json::array({"under_score"}),
                 Json::array({"../escape"}), Json::array({"two words"}), Json::array({"line\nbreak"}),
                 Json::array({std::string("a\0b", 3)}), Json::array({std::string(64, 'a')}),
                 Json::array({"visible", "other", "visible"})})) {
            auto invalid = baseline;
            invalid["observability"]["capture_query_projects"] = invalid_projects;
            reject_config(invalid);
        }
        for (const auto count : {1, 32}) {
            auto projects = Json::array({std::string(63, 'a')});
            for (int index = 1; index < count; ++index) projects.push_back("project-" + std::to_string(index));
            fixture.value = baseline;
            fixture.value["observability"]["capture_query_projects"] = projects;
            fixture.save();
            check(load_config(fixture.config_path).observability.capture_query_projects ==
                      projects.get<std::vector<std::string>>(), "capture project IDs or ordering changed");
            fixture.value["observability"]["max_retained_requests"] = 0;
            reject_config(fixture.value);
        }
        auto too_many = baseline;
        for (int index = 0; index < 33; ++index)
            too_many["observability"]["capture_query_projects"].push_back("project-" + std::to_string(index));
        reject_config(too_many);
        fixture.value = baseline;
        fixture.save();
        check(config.workers == 4 && config.database.pool_size == 4 && config.database.source_host_id == "local-test" &&
                  config.database.expected_server_id == "0123456789abcdef0123456789abcdef",
              "worker, source-host or server-identity binding lost");
        const Json recovery = {{"schema_version", "shin.server-recovery-config.v1"}, {"database", fixture.value["database"]}};
        fixture.write(fixture.recovery_path, recovery.dump());
        const auto recovery_config = load_recovery_config(fixture.recovery_path);
        check(recovery_config.pool_size == 1 && recovery_config.source_host_id == "recovery" &&
                  recovery_config.expected_server_id == "0123456789abcdef0123456789abcdef",
              "recovery configuration was not loaded as an offline store configuration");
        check(load_private_json(fixture.recovery_path, 65536) == recovery, "private JSON loader changed valid recovery input");
        auto invalid_recovery = recovery;
        invalid_recovery["database"].erase("server_id");
        fixture.write(fixture.recovery_path, invalid_recovery.dump());
        rejects("invalid-config", [&] { load_recovery_config(fixture.recovery_path); });
        invalid_recovery = recovery;
        invalid_recovery["database"]["server_id"] = "0123456789abcdef0123456789abcdeg";
        fixture.write(fixture.recovery_path, invalid_recovery.dump());
        rejects("invalid-config", [&] { load_recovery_config(fixture.recovery_path); });
        invalid_recovery = recovery;
        invalid_recovery["schema_version"] = "shin.server-recovery-config.v0";
        fixture.write(fixture.recovery_path, invalid_recovery.dump());
        rejects("invalid-config", [&] { load_recovery_config(fixture.recovery_path); });
        check(config.retrieval.routes.size() == 1 &&
                  config.retrieval.routes.front().workspace_id == "primary" &&
                  config.retrieval.routes.front().source_host_id == "local-test" &&
                  config.retrieval.routes.front().root == (fixture.root / "allowed" / "project").string() &&
                  !config.retrieval.routes.front().root_identity.empty() &&
                  config.retrieval.routes.front().profile_dir == fixture.root / "profiles" &&
                  config.retrieval.max_payload_bytes == 512ULL * 1024 * 1024 &&
                  config.retrieval.max_generation_bytes == 128ULL * 1024 * 1024 &&
                  config.retrieval.max_file_bytes == 64ULL * 1024 * 1024 && config.retrieval.max_generations == 8,
              "retrieval configuration was not loaded");
        check(!config.preparations.enabled && config.preparations.state_dir == fixture.preparations_path &&
                  config.preparations.max_retained_jobs == 8 && config.preparations.lease_seconds == 30,
              "preparation configuration was not loaded");
        check(authenticate(config, fixture.token).id == "owner", "valid credential rejected");
        rejects("unauthorized", [&] { authenticate(config, std::string(64, 'b')); });
        rejects("unauthorized", [&] { authenticate(config, "short"); });
        check(digest_hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA256 mismatch");
        rejects("invalid-json", [] { parse_bounded_json("{\"x\":1,\"x\":2}", 100); });
        rejects("invalid-json", [] { parse_bounded_json("{\"a\":{\"x\":1,\"x\":2}}", 100); });
        check(parse_bounded_json("{\"a\":{\"x\":1},\"b\":{\"x\":2}}", 100).size() == 2,
              "keys from sibling objects collided");
        rejects("invalid-json", [] { parse_bounded_json(std::string(32, '[') + "0" + std::string(32, ']'), 100); });
        rejects("invalid-json", [] { parse_bounded_json("{\"x\":NaN}", 100); });
        rejects("invalid-json", [] { parse_bounded_json("{}", 1); });
        const auto project = project_input(config, fixture.project());
        check(project.id == "visible" && project.display_name == "Visible project", "project input was not loaded");
        const auto workspace = workspace_input(config, fixture.workspace(fixture.root / "allowed" / "project"));
        check(workspace.source_host_id == "local-test" &&
                  workspace.root == (fixture.root / "allowed" / "project").string() && !workspace.root_identity.empty(),
              "workspace root binding missing");
        rejects("root-not-allowed", [&] { workspace_input(config, fixture.workspace(fixture.root / "allowed-neighbor")); });
        fs::create_directory_symlink(fixture.root / "allowed-neighbor", fixture.root / "allowed" / "escape");
        rejects("root-not-allowed", [&] { workspace_input(config, fixture.workspace(fixture.root / "allowed" / "escape")); });
        auto invalid_workspace = fixture.workspace(fixture.root / "allowed");
        invalid_workspace["root"] = fixture.root.string() + std::string(1, '\0');
        rejects("invalid-request", [&] { workspace_input(config, invalid_workspace); });
        auto invalid_project = fixture.project();
        invalid_project["root"] = (fixture.root / "allowed" / "project").string();
        rejects("invalid-request", [&] { project_input(config, invalid_project); });
        invalid_project = fixture.project();
        invalid_project["extra"] = true;
        rejects("invalid-request", [&] { project_input(config, invalid_project); });
        check(!valid_identifier("../escape") && !valid_identifier("Upper") && !valid_identifier(std::string(64, 'a')),
              "invalid identifier accepted");
        ::chmod(fixture.config_path.c_str(), 0644);
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.save();
        ::chmod(fixture.connection_path.c_str(), 0644);
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        ::chmod(fixture.connection_path.c_str(), 0600);
        fs::create_symlink(fixture.config_path, fixture.root / "config-link");
        rejects("invalid-config", [&] { load_config(fixture.root / "config-link"); });
        fixture.value["unknown"] = 1;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value.erase("unknown");
        fixture.value["listen"]["host"] = "0.0.0.0";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["listen"]["host"] = "127.0.0.1";
        fixture.value["workers"] = 17;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["workers"] = 4;
        fixture.value.erase("source_host_id");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["source_host_id"] = "Upper";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["source_host_id"] = "local-test";
        fixture.value["database"].erase("server_id");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["database"]["server_id"] = "0123456789abcdef0123456789abcdeg";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["database"]["server_id"] = "0123456789abcdef0123456789abcdef";
        fixture.value["schema_version"] = "shin.server-config.v8";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v1";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v2";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v3";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v4";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v5";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["schema_version"] = "shin.server-config.v6";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        auto legacy = fixture.value;
        legacy.erase("observability");
        reject_config(legacy);
        fixture.value = baseline;
        fixture.value.erase("preparations");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["enabled"] = "false";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["unknown"] = true;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["max_retained_jobs"] = 0;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"]["max_retained_jobs"] = 65;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["lease_seconds"] = 2;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"]["lease_seconds"] = 301;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["max_retained_jobs"] = 64;
        fixture.value["preparations"]["lease_seconds"] = 3;
        fixture.value["retrieval"]["max_generation_bytes"] = 16 * 1024 * 1024;
        fixture.value["retrieval"]["max_file_bytes"] = 16 * 1024 * 1024;
        fixture.save();
        check(load_config(fixture.config_path).preparations.max_retained_jobs == 64,
              "preparation upper bound rejected");
        fixture.value["retrieval"]["max_generation_bytes"] = 128 * 1024 * 1024;
        fixture.value["retrieval"]["max_file_bytes"] = 64 * 1024 * 1024;
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["max_retained_jobs"] = 9;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"]["state_dir"] = "/";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        ::chmod(fixture.preparations_path.c_str(), 0750);
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        ::chmod(fixture.preparations_path.c_str(), 0700);
        const auto preparations_target = fixture.root / "preparations-target";
        fs::create_directory(preparations_target);
        ::chmod(preparations_target.c_str(), 0700);
        const auto preparations_link = fixture.root / "preparations-link";
        fs::create_directory_symlink(preparations_target, preparations_link);
        fixture.value["preparations"]["state_dir"] = preparations_link.string();
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        const auto root_preparations = fixture.root / "allowed" / "project" / "preparations";
        ::chmod((fixture.root / "allowed").c_str(), 0700);
        fixture.value["preparations"]["state_dir"] = (fixture.root / "allowed").string();
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fs::create_directory(root_preparations);
        ::chmod(root_preparations.c_str(), 0700);
        fixture.value["preparations"]["state_dir"] = root_preparations.string();
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value["preparations"] = fixture.preparations(fixture.preparations_path);
        fixture.value.erase("retrieval");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"] = {
            {"routes", Json::array({fixture.route("visible-route")})},
            {"max_payload_bytes", 512 * 1024 * 1024},
            {"max_generation_bytes", 128 * 1024 * 1024},
            {"max_file_bytes", 64 * 1024 * 1024},
            {"max_generations", 8}
        };
        fixture.value["retrieval"]["unknown"] = true;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"].erase("unknown");
        fixture.value["retrieval"]["routes"][0].erase("workspace_id");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0].erase("profile");
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0]["profile"] = "Upper";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0]["profile_dir"] = "relative";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0]["profile_dir"] = "/";
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0]["out_dir"] = (fixture.root / "allowed" / "project").string();
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"][0] = fixture.route("visible-route");
        fixture.value["retrieval"]["routes"][0]["root"] = (fixture.root / "allowed-neighbor").string();
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"] = Json::array({
            fixture.route("visible-route"), fixture.route("visible-route")
        });
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"] = Json::array();
        for (std::size_t index = 0; index < 33; ++index) {
            fixture.value["retrieval"]["routes"].push_back(fixture.route("route-" + std::to_string(index)));
        }
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["routes"] = Json::array({fixture.route("visible-route")});
        fixture.value["retrieval"]["max_payload_bytes"] = 0;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_payload_bytes"] = 1024ULL * 1024 * 1024 + 1;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_payload_bytes"] = 10;
        fixture.value["retrieval"]["max_generation_bytes"] = 11;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_generation_bytes"] = 5;
        fixture.value["retrieval"]["max_file_bytes"] = 6;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_file_bytes"] = 5;
        fixture.value["retrieval"]["max_generations"] = 0;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_generations"] = 33;
        fixture.save();
        rejects("invalid-config", [&] { load_config(fixture.config_path); });
        fixture.value["retrieval"]["max_payload_bytes"] = 512 * 1024 * 1024;
        fixture.value["retrieval"]["max_generation_bytes"] = 128 * 1024 * 1024;
        fixture.value["retrieval"]["max_file_bytes"] = 64 * 1024 * 1024;
        fixture.value["retrieval"]["max_generations"] = 8;
        fixture.value["retrieval"]["routes"] = Json::array();
        fixture.save();
        check(load_config(fixture.config_path).retrieval.routes.empty(), "empty retrieval routes rejected");
        std::cout << "server configuration, authentication and root-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
