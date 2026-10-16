#include "shin/server/session_bootstrap.h"
#include "shin/server/config.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace shin::server {
namespace {
struct Member { std::string project; BootstrapSelector selector; std::string query; bool required; };

Json selector_json(const BootstrapSelector& selector) {
    return {{"kind", selector.kind == BootstrapSelectorKind::route ? "route" : "publication"}, {"id", selector.id}};
}

std::vector<Member> parse_members(const Json& request) {
    require_fields(request, {"schema_version", "task_id", "members", "max_evidence_bytes"});
    if (request.at("schema_version") != "shin.session-bootstrap-request.v2")
        throw ServiceError(400, "invalid-request", "Bootstrap request version is unsupported.");
    const auto task = require_text(request, "task_id", 128);
    if (task.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
        throw ServiceError(400, "invalid-request", "Task identity is invalid.");
    const auto& budget = request.at("max_evidence_bytes");
    const auto& input = request.at("members");
    if (!budget.is_number_integer() || budget < 1024 || budget > 131072 ||
        !input.is_array() || input.empty() || input.size() > 8)
        throw ServiceError(400, "invalid-request", "Bootstrap bounds are invalid.");
    std::set<std::tuple<std::string, BootstrapSelectorKind, std::string>> identities;
    std::vector<Member> members;
    for (const auto& item : input) {
        require_fields(item, {"project_id", "selector", "query", "required"});
        const auto& selector = item.at("selector");
        require_fields(selector, {"kind", "id"});
        const auto kind = require_text(selector, "kind", 16);
        if (kind != "route" && kind != "publication")
            throw ServiceError(400, "invalid-request", "Bootstrap selector kind is unsupported.");
        Member member{require_text(item, "project_id", 63),
                      {kind == "route" ? BootstrapSelectorKind::route : BootstrapSelectorKind::publication,
                       require_text(selector, "id", 63)}, require_text(item, "query", 2048), false};
        if (!valid_identifier(member.project) || !valid_identifier(member.selector.id) || !item.at("required").is_boolean() ||
            !identities.emplace(member.project, member.selector.kind, member.selector.id).second)
            throw ServiceError(400, "invalid-request", "Bootstrap member identity is invalid.");
        member.required = item.at("required").get<bool>();
        members.push_back(std::move(member));
    }
    if (std::none_of(members.begin(), members.end(), [](const Member& m) { return m.required; }))
        throw ServiceError(400, "invalid-request", "At least one required member is necessary.");
    std::stable_partition(members.begin(), members.end(), [](const Member& m) { return m.required; });
    return members;
}

bool matches_context(const Json& generation, const Json& context) {
    for (const auto* field : {"project_id", "route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision"})
        if (generation.at(field) != context.at(field)) return false;
    return generation.value("publication_id", Json(nullptr)) == context.value("publication_id", Json(nullptr));
}

bool matches_selector(const Member& member, const Json& context) {
    if (context.at("project_id") != member.project || context.at("admission") != "exact-current") return false;
    return member.selector.kind == BootstrapSelectorKind::publication ?
        context.value("publication_id", Json(nullptr)) == member.selector.id :
        !context.contains("publication_id") && context.at("route_id") == member.selector.id;
}
}

Json session_bootstrap(const Json& request, const BootstrapAccess& access) {
    const auto members = parse_members(request);
    // Optional members still require upfront project authorization/existence.
    // No earlier member may load evidence before this complete authorization pass.
    for (const auto& member : members) access.authorize(member.project);
    const auto maximum = request.at("max_evidence_bytes").get<std::size_t>();
    std::size_t bytes = 0;
    std::size_t member_bytes = 0;
    Json results = Json::array();
    bool contexts_ready = true;
    bool health_ready = true;
    for (const auto& member : members) {
        Json item{{"project_id", member.project}, {"selector", selector_json(member.selector)}, {"required", member.required},
            {"state", "unavailable"}, {"reason_code", nullptr}, {"context", nullptr}, {"health", nullptr},
            {"coverage_complete", false}, {"health_ready", false}, {"health_reason_code", nullptr}};
        try {
            auto context = access.retrieve(member.project, member.selector, member.query);
            if (!matches_selector(member, context))
                throw ServiceError(500, "bootstrap-context-binding-mismatch", "Bootstrap context does not match its selector.");
            const auto& evidence = context.at("evidence");
            const bool nonempty = !evidence.at("evidence").empty();
            const bool coverage = evidence.at("query_coverage").at("uncovered_group_count") == 0;
            item["coverage_complete"] = coverage;
            const auto size = context.dump().size();
            if (size > maximum - bytes) {
                item["state"] = "budget-exceeded";
                item["reason_code"] = "bootstrap-evidence-budget";
            } else {
                bytes += size;
                item["context"] = std::move(context);
                item["state"] = !nonempty ? "no-evidence" : coverage ? "ready" : "partial-coverage";
            }
        } catch (const ServiceError& error) {
            item["reason_code"] = error.code;
        }
        try {
            auto observed = access.health(member.project, member.selector);
            const bool agreement = item.at("context").is_null() ||
                matches_context(observed.at("current_generation"), item.at("context"));
            item["health_ready"] = observed.at("state") == "healthy" && observed.at("selection_aligned") == true && agreement;
            if (!agreement) item["health_reason_code"] = "bootstrap-selection-changed";
            item["health"] = std::move(observed);
        } catch (const ServiceError& error) {
            item["health_reason_code"] = error.code;
        }
        if (member.required) {
            contexts_ready &= item.at("state") == "ready";
            health_ready &= item.at("health_ready") == true;
        }
        const auto item_bytes = item.dump().size();
        if (item_bytes > 262144 - member_bytes)
            throw ServiceError(413, "bootstrap-response-too-large", "Narrow the session member set or evidence budget.");
        member_bytes += item_bytes;
        results.push_back(std::move(item));
    }
    Json result{{"schema_version", "shin.session-bootstrap.v2"}, {"task_id", request.at("task_id")},
        {"ready", contexts_ready && health_ready}, {"context_ready", contexts_ready}, {"health_ready", health_ready},
        {"members", std::move(results)}, {"evidence_bytes", bytes}, {"max_evidence_bytes", maximum},
        {"read_only", true}, {"snapshot_consistency", "independently-observed-members"}, {"checkpoint_restored", false}};
    if (result.dump().size() > 262144)
        throw ServiceError(413, "bootstrap-response-too-large", "Narrow the session member set or evidence budget.");
    return result;
}

} // namespace shin::server
