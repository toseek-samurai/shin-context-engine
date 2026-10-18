#include "shin/observer/model_usage.h"
#include "shin/observer/observer.h"
#include "shin/observer/local_supervision.h"
#include <atomic>
#include <csignal>
#include <iostream>
#include <set>
#include <thread>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv, char** environment) {
    using namespace shin::observer;
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << Json{{"tool","shin-usage"},{"version",SHIN_OBSERVER_VERSION},{"runtime","native-cpp"}}.dump() << '\n'; return 0;
        }
        fs::path config_path, home, registry; bool watch = false, definition = false;
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (!seen.insert(option).second) throw Error("usage-arguments", "Duplicate usage option.");
            if (option == "--watch") watch = true;
            else if (option == "--service-definition") definition = true;
            else if (i + 1 < argc && option == "--registry") registry = argv[++i];
            else if (i + 1 < argc && option == "--config") config_path = argv[++i];
            else if (i + 1 < argc && option == "--home") home = argv[++i];
            else throw Error("usage-arguments", "Use --config FILE --home PRIVATE_DIRECTORY [--watch | --service-definition].");
        }
        if (!registry.empty()) {
            if (!registry.is_absolute() || !home.empty() || watch || definition)
                throw Error("usage-arguments", "Use --registry ABSOLUTE_HOME [--config INITIAL_CONFIG].");
            const auto seed = config_path.empty() ? Json(nullptr) : parse_json(read_text(config_path,65536,true));
            std::cout << collect_registered_model_usage(registry,default_model_usage_sources(environment),seed).dump() << '\n';
            return 0;
        }
        if (watch && definition) throw Error("usage-arguments", "Choose watch or service definition.");
        if (!config_path.is_absolute() || !home.is_absolute()) throw Error("usage-arguments", "Absolute private paths required.");
        (void)directory_identity(config_path.parent_path(),true);
        const auto config = parse_json(read_text(config_path,65536,true));
        const auto pin = digest(config.dump());
        if (definition) {
            if (fs::canonical(home) != home || fs::canonical(config_path) != config_path)
                throw Error("unsafe-directory", "Scheduled collection requires canonical paths.");
            (void)directory_identity(home, true);
            StateStore history(home, false);
            const auto old = history.load();
            if (!old || old->at("config_sha256") != pin)
                throw Error("usage-config-changed", "Collect once with this configuration before scheduling it.");
            (void)summarize_model_usage(*old, now_ms());
            const auto binary = current_executable_path().string();
            const auto label = "org.shin.usage." + digest(home.string()).substr(0,32);
            const Json unit{{"Label",label},{"Program",binary},
                {"ProgramArguments",Json::array({binary,"--config",config_path.string(),"--home",home.string()})},
                {"WorkingDirectory","/"},{"EnvironmentVariables",{{"LC_ALL","C"}}},
                {"RunAtLoad",true},{"StartInterval",300},{"ThrottleInterval",60},{"ExitTimeOut",10},
                {"ProcessType","Background"},{"LowPriorityIO",true},{"Nice",5},{"Umask",63},
                {"StandardOutPath","/dev/null"},{"StandardErrorPath","/dev/null"}};
            std::cout << Json{{"ok",true},{"schema_version","shin.usage-service-definition.v1"},
                {"manager","launchd-user"},{"label",label},{"unit",unit},{"plist",native_launchd_plist(unit)},
                {"config_sha256",pin},{"installation_performed",false},{"collection_performed",false}}.dump() << '\n';
            return 0;
        }
        ensure_private_directory(home);
        StateStore store(home,true);
        auto old = store.load();
        if (old && old->at("config_sha256") != pin) throw Error("usage-config-changed", "Use a new private history home after changing attribution.");
        std::signal(SIGTERM,stop); std::signal(SIGINT,stop);
        do {
            auto result = collect_model_usage(config,now_ms());
            if (old) for (auto& item : result.at("sources").items()) if (item.value().at("state") == "unavailable") {
                const auto& client = item.key();
                Json retained = Json::array();
                for (const auto& row : result.at("buckets")) if (row.at("client") != client) retained.push_back(row);
                for (const auto& row : old->at("buckets")) if (row.at("client") == client) retained.push_back(row);
                result["buckets"] = std::move(retained);
                item.value()["last_success_unix_ms"] = old->at("sources").at(client).at("last_success_unix_ms");
            }
            (void)summarize_model_usage(result,now_ms()); store.save(result); old = result;
            std::cout << Json{{"ok",true},{"buckets",result.at("buckets").size()},{"sources",result.at("sources")}}.dump() << '\n' << std::flush;
            if (!watch) break;
            const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            while (!stopped && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (!stopped);
        return 0;
    } catch (const Error& error) {
        // Emit only the stable code, never source paths or exception details.
        try { std::cerr << Json{{"ok",false},{"error",error.code}}.dump() << '\n'; }
        catch (...) { std::cerr << "{\"ok\":false,\"error\":\"native-usage-collection-failed\"}\n"; }
        return 2;
    } catch (const std::exception&) {
        std::cerr << "{\"ok\":false,\"error\":\"native-usage-collection-failed\"}\n"; return 2;
    }
}
