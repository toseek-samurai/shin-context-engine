#include "context_internal.h"

namespace shin::context {
namespace {

constexpr std::uintmax_t kMaxRoutingConfigBytes = 1024U * 1024U;
constexpr std::size_t kMaxRoutingRoutes = 32;
constexpr std::size_t kMaxRouteSignals = 64;
constexpr std::size_t kMaxRouteSignalBytes = 256;
constexpr std::size_t kMaxRouteDescriptionBytes = 1024;
constexpr std::size_t kMaxRoutingQueryBytes = 16U * 1024U;
constexpr int kDefaultSuggestionLimit = 3;
constexpr int kMaxSuggestionLimit = 16;

struct SuggestProfileOptions {
    fs::path config;
    std::optional<fs::path> from_root;
    std::string query;
    int limit = kDefaultSuggestionLimit;
};

struct RoutingSignal {
    std::string value;
    std::vector<std::string> terms;
};

struct ProfileRoute {
    std::string id;
    std::string description;
    std::string profile_id;
    fs::path profile_dir;
    fs::path root;
    std::optional<std::string> marker;
    std::vector<RoutingSignal> signals;
};

struct ProfileRoutingConfig {
    std::string id;
    fs::path path;
    std::string content_hash;
    std::vector<ProfileRoute> routes;
};

struct RouteMatch {
    const ProfileRoute* route = nullptr;
    int score = 0;
    std::vector<std::string> matched_signals;
    std::vector<std::string> matched_query_terms;
    bool current_route = false;
};

bool safe_routing_id(std::string_view value) {
    return value.size() <= 64 && portable_identifier_valid(value);
}

void reject_unknown_routing_fields(const Json& object,
                                   const std::set<std::string>& allowed,
                                   const std::string& location) {
    if (!object.is_object()) {
        throw CliError("invalid-profile-routing-config", "configuration",
                       location + " must be a JSON object.", 2, false,
                       "Repair the explicit profile-routing config and retry.");
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (!allowed.contains(field.key())) {
            throw CliError("invalid-profile-routing-config", "configuration",
                           location + " contains unknown field: " + field.key() + ".",
                           2, false,
                           "Remove the unsupported field from the profile-routing config.");
        }
    }
}

fs::path resolve_routing_directory(const fs::path& base,
                                   const std::string& value,
                                   const std::string& field) {
    if (value.empty()) {
        throw CliError("invalid-profile-route", "configuration",
                       "Profile route is missing " + field + ".", 2, false,
                       "Provide every route root and profileDir explicitly.");
    }
    const fs::path candidate = fs::path(value).is_absolute() ? fs::path(value)
                                                              : base / value;
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(candidate, error);
    if (error || !fs::is_directory(canonical, error)) {
        throw CliError("invalid-profile-routing-path", "configuration",
                       "Profile route " + field + " is not an existing directory: " +
                           candidate.generic_string() + ".",
                       2, false, "Repair the explicit route path and retry.");
    }
    return canonical;
}

std::vector<RoutingSignal> parse_route_signals(const Json& route,
                                               const std::string& location) {
    const Json* values = find_json_field(route, "signals");
    if (values == nullptr || !values->is_array() || values->empty() ||
        values->size() > kMaxRouteSignals) {
        throw CliError("invalid-profile-route-signals", "configuration",
                       location + ".signals must contain 1-64 strings.", 2, false,
                       "Provide bounded, project-owned routing phrases.");
    }
    std::vector<RoutingSignal> signals;
    std::set<std::string> normalized_values;
    for (std::size_t index = 0; index < values->size(); ++index) {
        const Json& item = (*values)[index];
        if (!item.is_string()) {
            throw CliError("invalid-profile-route-signal", "configuration",
                           location + ".signals[" + std::to_string(index) +
                               "] must be a string.",
                           2, false, "Replace the signal with bounded text.");
        }
        const std::string value = trim(to_lower(item.get<std::string>()));
        if (value.empty() || value.size() > kMaxRouteSignalBytes) {
            throw CliError("invalid-profile-route-signal", "configuration",
                           location + ".signals[" + std::to_string(index) +
                               "] must contain 1-256 bytes.",
                           2, false, "Use one concise routing phrase per signal.");
        }
        if (!normalized_values.insert(value).second) {
            throw CliError("duplicate-profile-route-signal", "configuration",
                           location + " repeats routing signal: " + value + ".", 2, false,
                           "Keep each normalized route signal once.");
        }
        std::vector<std::string> terms = parse_terms(value);
        if (terms.empty()) {
            throw CliError("invalid-profile-route-signal", "configuration",
                           location + " has a signal without meaningful terms: " + value + ".",
                           2, false, "Use a concrete project, subsystem, or tool phrase.");
        }
        std::sort(terms.begin(), terms.end());
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        signals.push_back({value, std::move(terms)});
    }
    std::sort(signals.begin(), signals.end(), [](const auto& left, const auto& right) {
        return left.value < right.value;
    });
    return signals;
}

ProfileRoutingConfig load_profile_routing_config(const fs::path& input_path) {
    const fs::path path = fs::absolute(input_path).lexically_normal();
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw CliError("invalid-profile-routing-config", "configuration",
                       "Profile-routing config must be a regular non-symlink file: " +
                           path.generic_string() + ".",
                       2, false, "Provide a trusted local shin.profile-routing.v1 file.");
    }
    const std::uintmax_t bytes = fs::file_size(path, error);
    if (error) {
        throw CliError("invalid-profile-routing-config", "configuration",
                       "Cannot inspect profile-routing config: " + path.generic_string() + ".",
                       2, false, "Verify the config permissions and retry.");
    }
    if (bytes > kMaxRoutingConfigBytes) {
        throw CliError("profile-routing-config-too-large", "resource",
                       "Profile-routing config exceeds the 1 MiB limit.", 1, false,
                       "Reduce the reviewed route declaration.");
    }

    const Json document = parse_json_text(read_file(path), path.string());
    reject_unknown_routing_fields(document, {"schemaVersion", "id", "routes"},
                                  "Profile-routing config");
    if (json_string_value(document, "schemaVersion") != "shin.profile-routing.v1") {
        throw CliError("invalid-profile-routing-schema", "configuration",
                       "Profile-routing schemaVersion must be shin.profile-routing.v1.",
                       2, false, "Migrate the config to the supported routing schema.");
    }

    ProfileRoutingConfig config;
    config.id = json_string_value(document, "id");
    config.path = fs::weakly_canonical(path);
    if (!safe_routing_id(config.id)) {
        throw CliError("invalid-profile-routing-id", "configuration",
                       "Profile-routing id must use 1-64 portable identifier bytes.",
                       2, false, "Repair the routing config id.");
    }
    const Json* routes = find_json_field(document, "routes");
    if (routes == nullptr || !routes->is_array() || routes->empty() ||
        routes->size() > kMaxRoutingRoutes) {
        throw CliError("invalid-profile-routes", "configuration",
                       "Profile-routing config must contain 1-32 routes.", 2, false,
                       "Provide a bounded non-empty reviewed route set.");
    }

    const fs::path base = config.path.parent_path();
    std::set<std::string> route_ids;
    std::vector<fs::path> route_roots;
    for (std::size_t index = 0; index < routes->size(); ++index) {
        const Json& route_document = (*routes)[index];
        const std::string location = "routes[" + std::to_string(index) + "]";
        reject_unknown_routing_fields(
            route_document,
            {"id", "description", "profile", "profileDir", "root", "marker", "signals"},
            location);
        ProfileRoute route;
        route.id = json_string_value(route_document, "id");
        route.profile_id = json_string_value(route_document, "profile");
        route.description = json_string_value(route_document, "description");
        if (!safe_routing_id(route.id) || !safe_routing_id(route.profile_id)) {
            throw CliError("invalid-profile-route", "configuration",
                           location + " id and profile must be portable identifiers.",
                           2, false, "Repair the route id and profile id.");
        }
        if (!route_ids.insert(route.id).second) {
            throw CliError("duplicate-profile-route", "configuration",
                           "Duplicate profile route id: " + route.id + ".", 2, false,
                           "Use one stable id per route.");
        }
        if (route.description.size() > kMaxRouteDescriptionBytes) {
            throw CliError("invalid-profile-route", "configuration",
                           location + ".description exceeds 1024 bytes.", 2, false,
                           "Use a concise route description.");
        }
        route.profile_dir = resolve_routing_directory(
            base, json_string_value(route_document, "profileDir"), "profileDir");
        route.root = resolve_routing_directory(
            base, json_string_value(route_document, "root"), "root");
        for (const fs::path& existing : route_roots) {
            std::error_code equivalent_error;
            const bool equivalent = fs::equivalent(existing, route.root, equivalent_error);
            if (equivalent_error) {
                throw CliError("invalid-profile-routing-path", "configuration",
                               "Cannot compare resolved profile route roots.", 2, false,
                               "Verify route directory permissions and retry.");
            }
            if (equivalent) {
                throw CliError("duplicate-profile-route-root", "configuration",
                               "Multiple routes resolve to the same root: " +
                                   route.root.generic_string() + ".",
                               2, false,
                               "Keep one default profile route per root; model archaeology separately.");
            }
        }
        route_roots.push_back(route.root);

        const auto marker = route_document.find("marker");
        if (marker != route_document.end()) {
            if (!marker->is_string() || !profile_relative_path_valid(marker->get<std::string>())) {
                throw CliError("invalid-profile-route-marker", "configuration",
                               location + ".marker must be a canonical repository-relative path.",
                               2, false, "Store only a portable marker path in routing config.");
            }
            route.marker = marker->get<std::string>();
            if (profile_path_contains_symlink(route.root, *route.marker)) {
                throw CliError("unsafe-profile-route-marker", "configuration",
                               location + ".marker traverses a symlink.", 2, false,
                               "Use a regular non-symlink marker below the route root.");
            }
            const fs::file_status marker_status =
                fs::symlink_status(route.root / *route.marker, error);
            if (error || !fs::is_regular_file(marker_status)) {
                throw CliError("missing-profile-route-marker", "configuration",
                               location + ".marker is not a regular file: " + *route.marker + ".",
                               2, false, "Repair or remove the reviewed route marker.");
            }
        }
        route.signals = parse_route_signals(route_document, location);
        const Profile profile = make_profile(route.profile_id, route.profile_dir);
        if (profile.bootstrap_only) {
            throw CliError("bootstrap-profile-route", "configuration",
                           location + " selects bootstrap-only profile " + route.profile_id + ".",
                           2, false, "Route only to reviewed permanent profiles.");
        }
        config.routes.push_back(std::move(route));
    }
    std::sort(config.routes.begin(), config.routes.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });

    Json canonical = {
        {"schema_version", "shin.profile-routing.v1"},
        {"id", config.id},
        {"routes", Json::array()}
    };
    for (const ProfileRoute& route : config.routes) {
        Json signals = Json::array();
        for (const RoutingSignal& signal : route.signals) {
            signals.push_back(signal.value);
        }
        Json marker = Json(nullptr);
        if (route.marker.has_value()) {
            marker = route.marker.value_or(std::string{});
        }
        canonical["routes"].push_back({
            {"id", route.id},
            {"description", route.description},
            {"profile", route.profile_id},
            {"profile_dir", route.profile_dir.generic_string()},
            {"root", route.root.generic_string()},
            {"marker", std::move(marker)},
            {"signals", std::move(signals)}
        });
    }
    config.content_hash = fnv1a64(canonical.dump());
    return config;
}

SuggestProfileOptions parse_suggest_profile_options(const std::vector<std::string>& args) {
    SuggestProfileOptions options;
    bool config_seen = false;
    bool from_root_seen = false;
    bool limit_seen = false;
    std::vector<std::string> query_parts;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& argument = args[index];
        auto read_value = [&](const std::string& option) -> const std::string& {
            if (index + 1 >= args.size()) {
                throw_usage_error("missing-option-value", option + " requires a value.");
            }
            return args[++index];
        };
        if (argument == "--config") {
            if (config_seen) {
                throw_usage_error("duplicate-option",
                                  "suggest-profile accepts --config only once.");
            }
            options.config = read_value(argument);
            config_seen = true;
        } else if (argument == "--from-root") {
            if (from_root_seen) {
                throw_usage_error("duplicate-option",
                                  "suggest-profile accepts --from-root only once.");
            }
            options.from_root = fs::path(read_value(argument));
            from_root_seen = true;
        } else if (argument == "--limit") {
            if (limit_seen) {
                throw_usage_error("duplicate-option",
                                  "suggest-profile accepts --limit only once.");
            }
            options.limit = parse_cli_integer(read_value(argument), "--limit", 1);
            if (options.limit > kMaxSuggestionLimit) {
                throw_usage_error("invalid-option-value",
                                  "--limit must be between 1 and 16.");
            }
            limit_seen = true;
        } else if (starts_with(argument, "--")) {
            throw_usage_error("unsupported-option",
                              "Unsupported suggest-profile option: " + argument + ".");
        } else {
            query_parts.push_back(argument);
        }
    }
    if (!config_seen) {
        throw_usage_error("missing-option", "suggest-profile requires --config FILE.");
    }
    for (std::size_t index = 0; index < query_parts.size(); ++index) {
        if (index > 0) {
            options.query.push_back(' ');
        }
        options.query += query_parts[index];
    }
    options.query = trim(options.query);
    if (options.query.empty()) {
        throw_usage_error("missing-query", "suggest-profile requires a task query.");
    }
    if (options.query.size() > kMaxRoutingQueryBytes) {
        throw CliError("profile-routing-query-too-large", "resource",
                       "Profile-routing query exceeds 16384 bytes.", 1, false,
                       "Use a shorter routing question.");
    }
    return options;
}

bool path_is_at_or_below(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    return relative == "." || profile_relative_path_valid(relative.generic_string(), true);
}

std::optional<fs::path> resolve_from_root(const std::optional<fs::path>& from_root) {
    if (!from_root.has_value()) {
        return std::nullopt;
    }
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(fs::absolute(*from_root), error);
    if (error || !fs::is_directory(canonical, error)) {
        throw CliError("invalid-profile-routing-from-root", "configuration",
                       "--from-root is not an existing directory: " +
                           from_root->generic_string() + ".",
                       2, false, "Pass the current project or component root explicitly.");
    }
    return canonical;
}

const ProfileRoute* find_current_route(const ProfileRoutingConfig& config,
                                       const std::optional<fs::path>& resolved_from_root) {
    if (!resolved_from_root.has_value()) {
        return nullptr;
    }
    const ProfileRoute* selected = nullptr;
    for (const ProfileRoute& route : config.routes) {
        if (!path_is_at_or_below(route.root, *resolved_from_root)) {
            continue;
        }
        if (selected == nullptr || route.root.generic_string().size() >
                                       selected->root.generic_string().size()) {
            selected = &route;
        }
    }
    return selected;
}

RouteMatch match_route(const ProfileRoute& route,
                       const std::set<std::string>& query_terms,
                       const ProfileRoute* current_route) {
    RouteMatch match;
    match.route = &route;
    match.current_route = current_route != nullptr && current_route->id == route.id;
    std::set<std::string> matched_terms;
    for (const RoutingSignal& signal : route.signals) {
        const bool matched = std::all_of(
            signal.terms.begin(), signal.terms.end(),
            [&](const std::string& term) { return query_terms.contains(term); });
        if (!matched) {
            continue;
        }
        match.matched_signals.push_back(signal.value);
        matched_terms.insert(signal.terms.begin(), signal.terms.end());
        match.score += 100 + static_cast<int>(signal.terms.size()) * 10;
    }
    match.matched_query_terms.assign(matched_terms.begin(), matched_terms.end());
    return match;
}

auto semantic_match_key(const RouteMatch& match) {
    return std::tuple(match.score, match.matched_query_terms.size(),
                      match.matched_signals.size());
}

bool route_match_better(const RouteMatch& left, const RouteMatch& right) {
    const auto left_key = semantic_match_key(left);
    const auto right_key = semantic_match_key(right);
    if (left_key != right_key) {
        return left_key > right_key;
    }
    return left.route->id < right.route->id;
}

Json route_identity_json(const ProfileRoute* route) {
    if (route == nullptr) {
        return nullptr;
    }
    return {
        {"route_id", route->id},
        {"profile", route->profile_id},
        {"profile_dir", route->profile_dir.generic_string()},
        {"root", route->root.generic_string()},
        {"marker", route->marker.has_value() ? Json(*route->marker) : Json(nullptr)}
    };
}

Json route_match_json(const RouteMatch& match) {
    return {
        {"route_id", match.route->id},
        {"description", match.route->description},
        {"profile", match.route->profile_id},
        {"profile_dir", match.route->profile_dir.generic_string()},
        {"root", match.route->root.generic_string()},
        {"marker", match.route->marker.has_value() ? Json(*match.route->marker)
                                                    : Json(nullptr)},
        {"score", match.score},
        {"matched_signals", match.matched_signals},
        {"matched_query_terms", match.matched_query_terms},
        {"current_route", match.current_route}
    };
}

}  // namespace

int run_suggest_profile(const std::vector<std::string>& args) {
    const SuggestProfileOptions options = parse_suggest_profile_options(args);
    const ProfileRoutingConfig config = load_profile_routing_config(options.config);
    const std::optional<fs::path> resolved_from_root = resolve_from_root(options.from_root);
    const ProfileRoute* current_route = find_current_route(config, resolved_from_root);
    std::vector<std::string> parsed_query_terms = parse_terms(options.query);
    std::sort(parsed_query_terms.begin(), parsed_query_terms.end());
    parsed_query_terms.erase(
        std::unique(parsed_query_terms.begin(), parsed_query_terms.end()),
        parsed_query_terms.end());
    const std::set<std::string> query_terms(parsed_query_terms.begin(),
                                            parsed_query_terms.end());

    std::vector<RouteMatch> matches;
    for (const ProfileRoute& route : config.routes) {
        RouteMatch match = match_route(route, query_terms, current_route);
        if (match.score > 0) {
            matches.push_back(std::move(match));
        }
    }
    std::sort(matches.begin(), matches.end(), route_match_better);

    const RouteMatch* recommendation = nullptr;
    std::string state = "no-match";
    if (!matches.empty()) {
        const bool unique_best = matches.size() == 1 ||
                                 semantic_match_key(matches[0]) !=
                                     semantic_match_key(matches[1]);
        if (unique_best) {
            recommendation = &matches[0];
            state = recommendation->current_route ? "current-route" : "recommended";
        } else {
            state = "ambiguous";
        }
    }

    Json candidates = Json::array();
    for (std::size_t index = 0;
         index < std::min<std::size_t>(matches.size(), options.limit); ++index) {
        candidates.push_back(route_match_json(matches[index]));
    }
    Json output = make_result_document("shin.profile-suggestion.v1",
                                       "suggest-profile", true);
    output["state"] = state;
    output["query"] = options.query;
    output["query_terms"] = parsed_query_terms;
    output["config"] = {
        {"id", config.id},
        {"path", config.path.generic_string()},
        {"content_hash", config.content_hash},
        {"routes", config.routes.size()}
    };
    output["policy"] = {
        {"matching", "explicit-signal-all-terms-v1"},
        {"automatic_switch", false},
        {"index_access", false},
        {"project_mutation", false},
        {"tie_policy", "ambiguous-no-recommendation"}
    };
    output["from_root"] = resolved_from_root.has_value()
                              ? Json(resolved_from_root->generic_string())
                              : Json(nullptr);
    output["current_route"] = route_identity_json(current_route);
    output["recommendation"] = recommendation == nullptr
                                     ? Json(nullptr)
                                     : route_match_json(*recommendation);
    output["switch_recommended"] =
        recommendation != nullptr && current_route != nullptr &&
        recommendation->route->id != current_route->id;
    output["matched_routes"] = matches.size();
    output["returned_routes"] = candidates.size();
    output["limit"] = options.limit;
    output["candidates"] = std::move(candidates);
    emit_json_document(std::cout, output);
    return 0;
}

}  // namespace shin::context
