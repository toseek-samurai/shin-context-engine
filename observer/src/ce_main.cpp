#include "shin/observer/local_project.h"
#include "shin/observer/local_identity.h"
#include "shin/observer/local_status.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_supervision.h"
#include "shin/observer/local_publication.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/local_updates.h"
#include "shin/observer/local_client_setup.h"
#include "shin/observer/model_usage.h"
#include "context_internal.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <csignal>
#ifndef _WIN32
#include "../tui/view.h"
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
using namespace shin::observer;
volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }
const char* user_home(char** environment) {
#ifdef _WIN32
    constexpr std::string_view key = "USERPROFILE=";
#else
    constexpr std::string_view key = "HOME=";
#endif
    for (auto** entry = environment; entry && *entry; ++entry)
        if (std::string_view(*entry).starts_with(key)) return *entry + key.size();
    throw Error("home-unavailable", "A user home is required or pass --home.");
}
fs::path executable_path() {
    return current_executable_path();
}
std::optional<fs::path> bundled_profiles() {
    const auto bin = executable_path().parent_path();
    for (const auto& prefix : {bin, bin.parent_path()}) {
        const auto registry = prefix / "share/shin-context/profiles";
        if (fs::is_regular_file(registry / "generic-source.json")) return fs::canonical(registry);
    }
    return std::nullopt;
}

int run(int argc, char** argv, char** environment) {
#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "_scm-host") return windows_scm_dispatch();
    if (argc == 3 && std::string_view(argv[1]) == "_scm-user") return windows_scm_user(argv[2]);
    if (argc == 2 && (std::string_view(argv[1]) == "_scm-install" || std::string_view(argv[1]) == "_scm-uninstall" ||
                     std::string_view(argv[1]) == "_scm-stop")) {
        const auto action = std::string_view(argv[1]) == "_scm-install" ? WindowsScmAction::install :
            std::string_view(argv[1]) == "_scm-stop" ? WindowsScmAction::stop : WindowsScmAction::remove;
        std::cout << windows_scm_setup(action, executable_path()).dump() << '\n';
        return 0;
    }
#endif
    if (argc == 2 && std::string_view(argv[1]) == "_service-worker") return local_service_worker();
    if (argc == 2 && std::string_view(argv[1]) == "_refresh-worker") return local_refresh_worker();
    if (argc == 2 && std::string_view(argv[1]) == "_resident-worker") return local_resident_worker();
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << Json{{"tool", "shin-ce"}, {"version", SHIN_OBSERVER_VERSION},
                         {"core_version", SHIN_VERSION}, {"offline", true}}.dump() << '\n';
        return 0;
    }
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "Connect a project once. Existing settings are reused; new projects refresh in the background.\n"
#ifndef _WIN32
                     "shin-ce stats [--home DIR] [--usage-file FILE] [--language auto|en|ru] opens the interactive read-only monitor.\n"
#endif
                     "shin-ce update-check|update-status [--prefix DIR] checks public releases or the last update result (macOS).\n"
                     "shin-ce update-install --revision HASH [--prefix DIR] installs an explicitly reviewed release (macOS).\n"
                     "shin-ce install [--prefix DIR] [--bin-dir DIR] [--home DIR] installs an extracted native kit.\n"
                     "shin-ce update [--prefix DIR] [--adopt-existing --home DIR --bin-dir DIR] updates from an extracted kit.\n"
                     "shin-ce installation|uninstall [--prefix DIR] verifies or removes owned kit files; project data stays.\n"
                     "shin-ce init [--root DIR]\nshin-ce project-add --root DIR [--home DIR] connects a folder without changing its files.\nshin-ce storage-status [--home DIR] [--root DIR] previews external storage.\nshin-ce storage-clean --revision HASH [--home DIR] [--root DIR] clears verified detached caches.\nshin-ce storage-policy --auto-clean on|off [--home DIR] controls detached-cache cleanup.\n"
                     "shin-ce status [--root DIR] [--home DIR] [--text]\n"
                     "shin-ce client-targets lists supported agents and their settings paths.\n"
                     "shin-ce client-connect --client NAME [--config FILE] [--root DIR] [--home DIR] previews a connection; --revision HASH applies that exact review.\n"
                     "shin-ce mcp-config --client codex|opencode|rider|generic [--root DIR] [--home DIR] [--text] prints settings without editing files.\n"
                     "shin-ce detach [--root DIR] [--home DIR] disconnects one project; data and history stay.\n"
                     "shin-ce query TEXT [--root DIR]\n"
                     "Project reads/status accept --session-id ID and --model-id ID (host-provided only); read/inspect accept --parent-request-id ID.\n"
                     "Read commands accept --diagnostic to label operator/test traffic in telemetry.\n"
                     "Advanced setup: init --observer-config FILE [--marker FILE] connects an existing managed index.\n"
                     "Add --manage-index once to authorize automatic publication of that existing index.\n"
                     "shin-ce refresh-release [--root DIR] [--home DIR] returns it to read-only attachment.\n"
                     "shin-ce profile-review [--root DIR] [--home HOME] previews a pending profile change.\n"
                     "shin-ce profile-accept HASH [--root DIR] [--home DIR] explicitly approves a reviewed profile revision.\n"
                     "shin-ce marker-accept SHA256 [--root DIR] [--home DIR] explicitly approves a reviewed marker revision.\n"
                     "shin-ce config-accept SHA256 [--root DIR] [--home DIR] approves same-route operational settings after refresh-release.\n"
                     "Init options: --offline, --no-service, --home DIR, --profile-dir DIR (new local projects only).\n"
                     "shin-ce watch [--root DIR ...] [--home DIR] [--samples N]\n"
                     "shin-ce serve --root DIR [--root DIR ...] [--home DIR]\n"
                     "shin-ce serve --registered [--home DIR]\n"
                     "shin-ce service-start|service-status|service-stop [--home DIR]\n"
                     "shin-ce service-status --text [--home DIR] shows a read-only headless overview.\n"
                     "Kit updates show progress on a terminal; add --progress for readable stderr in logs.\n"
                     "shin-ce service-definition [--home DIR] (read-only OS manager definition)\n"
                     "shin-ce service-install|service-update|service-uninstall|service-installation [--home DIR]\n"
                     "Registered projects use the resident service automatically; no silent fallback.\n"
                     "Read commands also accept --service to require an explicit-root service.\n"
                     "shin-ce identity [--root DIR] [--home DIR] (read-only local identity, not login)\n"
                     "shin-ce query TEXT [--root DIR] [--home DIR]\n"
                     "shin-ce inspect PATH --snapshot ID [--root DIR] [--home DIR]\n"
                     "shin-ce read PATH --snapshot ID [--root DIR] [--home DIR]\n"
                     "shin-ce read --record ID --snapshot ID [--root DIR] [--home DIR]\n";
        return 0;
    }
    LocalProjectRequest request;
    request.command = argv[1];
#ifdef __APPLE__
    if (request.command == "update-check" || request.command == "update-download" ||
        request.command == "update-install" || request.command == "update-status" || request.command == "update-acknowledge") {
        const char* home = user_home(environment);
        if (!home || !*home) throw Error("home-unavailable", "HOME is required for release updates.");
        auto result = local_updates(argc, argv, fs::canonical(home));
        result["tool"] = "shin-ce"; result["tool_version"] = SHIN_OBSERVER_VERSION;
        std::cout << result.dump(2) << '\n';
        return result.value("ok",false) ? 0 : 1;
    }
#endif
    if (request.command == "install" || request.command == "update" || request.command == "installation" || request.command == "uninstall") {
        const char* home = user_home(environment);
        if (!home || !*home) throw Error("home-unavailable", "HOME is required for a per-user installation.");
        auto result = local_installation(argc, argv, fs::canonical(home));
        result["schema_version"] = "shin.local-install-response.v1";
        result["tool"] = "shin-ce";
        result["tool_version"] = SHIN_OBSERVER_VERSION;
        result["readiness_assessed"] = result.value("readiness_assessed", false);
        std::cout << result.dump(2) << '\n';
        return result.at("ok") == true ? 0 : 1;
    }
    const bool background_child = request.command == "_serve-registered";
    const bool supervised_child = request.command == "_serve-supervised";
    bool registered = background_child || supervised_child;
    bool no_service = false;
    bool text_status = false;
    std::string setup_client = "generic", client_revision;
    fs::path client_config;
    fs::path usage_file;
    if (background_child || supervised_child) request.command = "serve";
    if (!std::set<std::string>{"client-targets", "client-connect", "mcp-config", "stats", "init", "project-add", "storage-status", "storage-clean", "storage-policy", "attach", "detach", "status", "identity", "query", "inspect", "read", "watch", "serve", "refresh-release", "profile-accept", "profile-review", "marker-accept", "config-accept",
                               "service-start", "service-status", "service-stop", "service-definition",
                               "service-install", "service-update", "service-uninstall", "service-installation"}.contains(request.command))
        throw Error("invalid-arguments", "Unsupported command; see shin-ce --help.");
    std::string ui_language = "auto";
    std::string storage_revision; std::optional<bool> storage_automatic;
    std::uint64_t samples = 0;
    std::vector<fs::path> watch_roots;
    bool use_service = request.command == "service-status" || request.command == "service-stop";
    request.root = fs::current_path();
    std::set<std::string> seen;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if ((request.command == "mcp-config" || request.command == "client-connect") && option == "--client" && seen.insert(option).second && i + 1 < argc) {
            setup_client = argv[++i];
            continue;
        }
        if (request.command == "client-connect" && (option == "--config" || option == "--revision") && seen.insert(option).second && i + 1 < argc) {
            if (option == "--config") client_config = argv[++i];
            else client_revision = argv[++i];
            continue;
        }
        if (request.command == "stats" && option == "--language" && seen.insert(option).second && i + 1 < argc) {
            ui_language = argv[++i];
            if (ui_language != "auto" && ui_language != "en" && ui_language != "ru")
                throw Error("invalid-arguments", "Language must be auto, en or ru.");
            continue;
        }
        if (request.command == "stats" && option == "--usage-file" && seen.insert(option).second && i + 1 < argc) {
            usage_file = argv[++i];
            if (!usage_file.is_absolute()) throw Error("invalid-arguments", "Use an absolute usage snapshot path.");
            continue;
        }
        if (request.command == "stats" && option != "--home")
            throw Error("invalid-arguments", "Use stats [--home DIR] [--usage-file FILE] [--language auto|en|ru].");
        if (option == "--text" && (request.command == "status" || request.command == "service-status" || request.command == "mcp-config") &&
            seen.insert(option).second) { text_status = true; continue; }
        if ((option == "--session-id" || option == "--model-id" || option == "--parent-request-id") &&
            std::set<std::string>{"query", "inspect", "read", "status"}.contains(request.command) &&
            seen.insert(option).second && i + 1 < argc) {
            const std::string value = argv[++i];
            if (option == "--session-id") {
                if (value.empty() || value.size() > 128) throw Error("invalid-arguments", "Session ID must contain 1 to 128 bytes.");
                request.session_id = value;
            } else if (option == "--model-id") {
                if (!valid_reported_model(value)) throw Error("invalid-arguments", "Invalid model ID.");
                request.model_id = value;
            } else {
                if ((request.command != "read" && request.command != "inspect") || value.size() != 64 ||
                    value.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw Error("invalid-arguments", "Parent must be the query's opaque request ID for read/inspect.");
                request.parent_request_id = value;
            }
            continue;
        }
        if (option == "--diagnostic" && std::set<std::string>{"query", "inspect", "read"}.contains(request.command)
            && seen.insert(option).second) { request.diagnostic = true; continue; }
        if (option == "--manage-index" && request.command == "init" && seen.insert(option).second) {
            request.manage_index = true; continue;
        }
        if (option == "--no-service" && request.command == "init" && seen.insert(option).second) {
            no_service = true; continue;
        }
        if (option == "--registered" && request.command == "serve" && !registered && seen.insert(option).second) {
            registered = true; continue;
        }
        if (option == "--root" && (request.command == "watch" || request.command == "serve")) {
            if (i + 1 == argc || watch_roots.size() == 16)
                throw Error("invalid-arguments", "Watch requires a root value and permits at most 16 projects.");
            watch_roots.emplace_back(argv[++i]);
            continue;
        }
        if (option == "--service" && std::set<std::string>{"status", "query", "inspect", "read"}.contains(request.command)
            && seen.insert(option).second) { use_service = true; continue; }
        if (option == "--samples" && request.command == "watch") {
            if (!seen.insert(option).second || i + 1 == argc)
                throw Error("invalid-arguments", "Sample count is duplicated or missing.");
            const std::string value = argv[++i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || samples == 0 || samples > 3600)
                throw Error("invalid-arguments", "Sample count must be 1..3600; omit it for continuous watching.");
            continue;
        }
        if ((option == "--revision" && request.command == "storage-clean") ||
            (option == "--auto-clean" && request.command == "storage-policy")) {
            if (!seen.insert(option).second || i + 1 == argc) throw Error("invalid-arguments", "Storage option is missing or duplicated.");
            const std::string value = argv[++i];
            if (option == "--revision") {
                if (value.size()!=64 || value.find_first_not_of("0123456789abcdef")!=std::string::npos) throw Error("invalid-arguments", "A valid preview revision is required.");
                storage_revision = value;
            } else {
                if (value != "on" && value != "off") throw Error("invalid-arguments", "Auto-clean must be on or off.");
                storage_automatic = value == "on";
            }
            continue;
        }
        const bool followup = request.command == "inspect" || request.command == "read";
        if ((option == "--snapshot" && followup) || (option == "--record" && request.command == "read")) {
            if (!seen.insert(option).second || i + 1 == argc)
                throw Error("invalid-arguments", "Follow-up option is duplicated or missing its value.");
            if (option == "--snapshot") request.snapshot = argv[++i];
            else request.record = argv[++i];
            continue;
        }
        if (option == "--offline" && request.command == "init" && seen.insert(option).second) continue;
        if ((option == "--root" && request.command != "service-definition") || option == "--home" || (option == "--profile-dir" && (request.command == "init" || request.command == "project-add")) ||
            ((option == "--observer-config" || option == "--marker") &&
             (request.command == "attach" || request.command == "init"))) {
            if (!seen.insert(option).second || i + 1 == argc)
                throw Error("invalid-arguments", "Path option is duplicated or missing its value.");
            const fs::path value = argv[++i];
            if (option == "--root") request.root = value;
            else if (option == "--home") request.home = value;
            else if (option == "--observer-config") request.observer_config = value;
            else if (option == "--marker") request.marker = value.string();
            else { request.profile_dir = value; request.profile_dir_explicit = true; }
        } else if (request.command == "query" && request.query.empty() && !option.starts_with("--")) {
            request.query = option;
        } else if (option == "--review-token" && request.command == "profile-accept" && seen.insert(option).second && i + 1 < argc) {
            request.profile_review_token = argv[++i];
        } else if (request.command == "profile-accept" && request.profile_revision.empty() && !option.starts_with("--")) {
            request.profile_revision = option;
        } else if (request.command == "marker-accept" && request.marker_revision.empty() && !option.starts_with("--")) {
            request.marker_revision = option;
        } else if (request.command == "config-accept" && request.config_revision.empty() && !option.starts_with("--")) {
            request.config_revision = option;
        } else if (followup && request.path.empty() && !option.starts_with("--")) {
            request.path = option;
        } else throw Error("invalid-arguments", "Unsupported command argument; see shin-ce --help.");
    }
    std::unique_ptr<LocalInstallationGuard> installation_guard;
    if (!std::set<std::string>{"client-targets", "client-connect", "mcp-config", "stats", "query", "read", "inspect", "status", "profile-review", "identity", "storage-status", "service-status", "service-installation", "service-definition"}.contains(request.command))
        installation_guard = lock_local_installation(executable_path(), background_child || supervised_child);
    if (const auto installed_home = installed_local_home(executable_path(), background_child || supervised_child)) {
        if (!request.home.empty() && fs::absolute(request.home).lexically_normal() != *installed_home)
            throw Error("installation-home-conflict", "This installed kit uses its recorded data home; a different --home would create a competing service.");
        request.home = *installed_home;
    }
    if (request.home.empty()) {
        const char* home = user_home(environment);
        if (home == nullptr || *home == '\0') throw Error("home-unavailable", "HOME is required or pass --home.");
        request.home = fs::canonical(home) / ".shin-ce";
    }
    if (!request.model_id.empty() && request.session_id.empty())
        throw Error("invalid-arguments", "Model attribution requires --session-id from the current host session.");
    if (!request.session_id.empty())
        request.session_id = digest("shin-session-v1:" + fs::weakly_canonical(request.home).string() + ":" + request.session_id);
    if (request.command == "client-targets" || request.command == "client-connect") {
        const auto targets = (request.command == "client-targets" || client_config.empty()) ? local_client_targets(fs::canonical(user_home(environment)), environment) : Json::object();
        if (request.command == "client-targets") { std::cout << targets.dump(2) << '\n'; return 0; }
        if (client_config.empty()) {
            for (const auto& target : targets.at("targets")) if (target.at("client") == setup_client) {
                client_config = target.at("config").get<std::string>(); break;
            }
        }
        std::cout << local_client_connect(executable_path(), request.root, request.home, setup_client, client_config, client_revision).dump(2) << '\n';
        return 0;
    }
    if (request.command == "mcp-config") {
        const auto setup = local_client_setup(executable_path(), request.root, request.home, setup_client);
        std::cout << (text_status ? setup.at("configuration").get<std::string>() : setup.dump(2) + "\n");
        return 0;
    }
    if ((request.command == "init" || request.command == "project-add") && !request.profile_dir) request.profile_dir = bundled_profiles();
    if (request.command == "stats") {
#ifndef _WIN32
        request.home = fs::absolute(request.home).lexically_normal();
        return tui::run(request, usage_file, ui_language);
#else
        throw Error("tui-platform-unsupported", "The terminal monitor currently supports macOS and Linux.");
#endif
    }
    if (!std::set<std::string>{"query", "read", "inspect", "status", "profile-review", "identity", "storage-status", "service-status", "service-installation", "service-definition"}.contains(request.command))
        check_local_installation_home(request.home, executable_path(), background_child || supervised_child);
    if (!use_service && std::set<std::string>{"status", "query", "inspect", "read"}.contains(request.command) &&
        fs::exists(fs::symlink_status(request.home / "registry"))) {
        const auto projects = registered_local_projects(request.home);
        const auto root = fs::canonical(request.root);
        use_service = std::any_of(projects.begin(), projects.end(), [&](const auto& project) { return project.root == root; });
    }
    if (request.command == "serve" && (registered == !watch_roots.empty()))
        throw Error("invalid-arguments", "Select either explicit roots or --registered for service startup.");
    if (background_child) {
        request.home = fs::absolute(request.home).lexically_normal();
#ifndef _WIN32
        if (::setsid() < 0 || ::chdir("/") != 0) throw Error("service-start-failed", "Cannot detach the native service session.");
#endif
    }
    if (request.command == "watch" || request.command == "serve") {
        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
    }
    Json result;
    if (request.command == "identity") result = {{"schema_version","shin.ce-response.v1"}, {"tool","shin-ce"},
        {"version",SHIN_OBSERVER_VERSION}, {"core_version",SHIN_VERSION}, {"command","identity"}, {"ok",true},
        {"identity",local_identity(request,false)}, {"delivery_performed",false}, {"consumer_mutated",false}};
    else if (request.command == "service-definition") result = local_service_definition(request, executable_path());
    else if (request.command == "service-install" || request.command == "service-uninstall")
        result = configure_local_autostart(request, executable_path());
    else if (request.command == "service-installation") result = local_autostart_status(request, executable_path());
    else if (request.command == "service-update") result = update_local_autostart(request, executable_path());
    else if (request.command == "serve")
        result = serve_local_projects(request, watch_roots, executable_path(), [] { return stop_requested != 0; }, registered, supervised_child,
                                      [&] { installation_guard.reset(); }
#ifndef _WIN32
                                      , [&]() -> Json {
                                          if (!registered) return nullptr;
                                          try { return default_model_usage_sources(environment); }
                                          catch (...) { return Json::object(); }
                                      }()
#endif
                                      );
    else if (request.command == "service-start") result = start_local_service(request, executable_path());
    else if (request.command.starts_with("storage-")) {
        if (request.command == "storage-clean" && storage_revision.empty()) throw Error("invalid-arguments", "Review storage-status and supply --revision.");
        if (request.command == "storage-policy" && !storage_automatic) throw Error("invalid-arguments", "Choose --auto-clean on or off.");
        result = local_storage(request.home, seen.contains("--root") ? std::optional<fs::path>(fs::absolute(request.root).lexically_normal()) : std::nullopt,
            storage_revision, storage_automatic);
    }
    else if (request.command == "project-add") {
        guard_project_storage(fs::absolute(request.home).lexically_normal(),request.root);
        request.command = "init";
        result = initialize_local_service(request, executable_path());
    }
    else if (request.command == "detach") {
        guard_project_storage(fs::absolute(request.home).lexically_normal(),request.root);
        result = detach_local_service(request);
    }
    else if (request.command == "init" && !no_service) result = initialize_local_service(request, executable_path());
    else if (request.command == "watch")
        result = watch_local_projects(request, watch_roots.empty() ? std::vector<fs::path>{request.root} : watch_roots,
                                      samples, [] { return stop_requested != 0; });
    else if (request.command == "service-stop") {
        const auto lock = lock_local_service_control(request.home);
        result = request_local_service(request);
    }
    else if (use_service) result = request_local_service(request);
    else result = local_project(request);
    if (request.command == "init" && result.value("ok", false) && !no_service) {
#ifndef _WIN32
        result["model_usage"] = {{"collection","automatic-background"},{"snapshot",(request.home/"usage/state.json").string()},
                                 {"history_completeness","depends-on-client-history"}};
#endif
    }
    // The service-status projection budgets diagnostic detail, not indentation.
    // Project/query and control-response limits stay unchanged.
    const auto output = request.command == "service-status" ? result.dump() : result.dump(2);
    if (output.size() + 1 > 65536) throw Error("output-limit", "Local response exceeds its byte limit.");
    if (text_status) std::cout << local_status_text(result);
    else std::cout << output << '\n';
    return result.at("ok") == true ? 0 : 1;
}

int failure(bool text_status, const char* code, const char* message, const Json& diagnostic = nullptr) noexcept {
    try {
        const Json result{{"schema_version", "shin.ce-response.v1"}, {"tool", "shin-ce"},
            {"ok", false}, {"error", error_json(Error(code, message, diagnostic))}};
        if (text_status) std::cerr << local_status_text(result);
        else std::cerr << result.dump() << '\n';
    } catch (...) {
        std::fputs("{\"schema_version\":\"shin.ce-response.v1\",\"tool\":\"shin-ce\",\"ok\":false,"
                   "\"error\":{\"code\":\"local-project-failed\",\"message\":\"Local project operation failed.\"}}\n", stderr);
    }
    return 2;
}
}

int main(int argc, char** argv, char** environment) noexcept {
    // New local artifacts must not inherit a group-writable shell umask.
#ifndef _WIN32
    ::umask(0077);
#endif
    bool text_status = false;
    if (argc > 1 && (std::string_view(argv[1]) == "status" || std::string_view(argv[1]) == "service-status"))
        for (int i = 2; i < argc; ++i) if (std::string_view(argv[i]) == "--text") text_status = true;
    try { return run(argc, argv, environment); }
    catch (const shin::observer::Error& error) {
        return failure(text_status, error.code.c_str(), error.what(), error.diagnostic);
    } catch (const shin::context::CliError& error) {
        return failure(text_status, error.code().c_str(), error.what());
    } catch (...) {
        return failure(text_status, "local-project-failed", "Local project operation failed; verify paths and binding.");
    }
}
