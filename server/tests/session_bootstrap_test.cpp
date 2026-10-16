#include "shin/server/session_bootstrap.h"

#include <cstddef>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace shin::server;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_error(int status, const char* code, const std::function<void()>& action) {
    try {
        action();
    } catch (const ServiceError& error) {
        check(error.status == status && error.code == code, "session bootstrap returned the wrong error");
        return;
    }
    throw std::runtime_error("session bootstrap accepted an invalid request");
}

const char* kind_name(BootstrapSelectorKind kind) {
    return kind == BootstrapSelectorKind::route ? "route" : "publication";
}

Json selector(std::string kind, std::string id) {
    return {{"kind", std::move(kind)}, {"id", std::move(id)}};
}

Json member(std::string project, std::string kind, std::string id, std::string query, bool required) {
    return {{"project_id", std::move(project)}, {"selector", selector(std::move(kind), std::move(id))},
            {"query", std::move(query)}, {"required", required}};
}

Json request(Json members, Json budget = 4096, std::string task_id = "session-bootstrap-test",
             Json schema_version = "shin.session-bootstrap-request.v2") {
    return {{"schema_version", std::move(schema_version)}, {"task_id", std::move(task_id)},
            {"members", std::move(members)}, {"max_evidence_bytes", std::move(budget)}};
}

Json brief(const std::string& project, const std::string& source_route, int revision = 7, int uncovered_groups = 0,
           Json records = Json::array(), std::string publication = {}) {
    Json context{{"project_id", project}, {"workspace_id", project + "-workspace"}, {"source_host_id", "local-test"},
                 {"route_id", source_route}, {"snapshot_id", project + "-snapshot"}, {"preparation_id", 101},
                 {"revision", revision}, {"admission", "exact-current"},
                 {"evidence", {{"evidence", std::move(records)},
                               {"query_coverage", {{"uncovered_group_count", uncovered_groups}}}}}};
    if (!publication.empty()) context["publication_id"] = std::move(publication);
    return context;
}

Json health(const Json& context, bool selection_aligned = true, std::string state = "healthy") {
    Json generation{{"project_id", context.at("project_id")}, {"workspace_id", context.at("workspace_id")},
                    {"source_host_id", context.at("source_host_id")}, {"route_id", context.at("route_id")},
                    {"snapshot_id", context.at("snapshot_id")}, {"preparation_id", context.at("preparation_id")},
                    {"revision", context.at("revision")}};
    if (context.contains("publication_id")) generation["publication_id"] = context.at("publication_id");
    return {{"schema_version", "shin.knowledge-health.v1"}, {"state", std::move(state)},
            {"selection_aligned", selection_aligned}, {"current_generation", std::move(generation)}};
}

struct AccessProbe {
    int authorize_calls = 0;
    int retrieve_calls = 0;
    int health_calls = 0;
    std::vector<std::string> calls;
    std::function<void(const std::string&)> authorize_action;
    std::function<Json(const std::string&, const BootstrapSelector&, const std::string&)> retrieve_action;
    std::function<Json(const std::string&, const BootstrapSelector&)> health_action;

    BootstrapAccess access() {
        return {
            [this](const std::string& project) {
                ++authorize_calls;
                calls.push_back("authorize:" + project);
                if (authorize_action) authorize_action(project);
            },
            [this](const std::string& project, const BootstrapSelector& selected, const std::string& query) {
                ++retrieve_calls;
                calls.push_back("retrieve:" + project + "/" + kind_name(selected.kind) + "/" + selected.id + "/" + query);
                if (!retrieve_action) throw std::runtime_error("unexpected retrieval callback");
                return retrieve_action(project, selected, query);
            },
            [this](const std::string& project, const BootstrapSelector& selected) {
                ++health_calls;
                calls.push_back("health:" + project + "/" + kind_name(selected.kind) + "/" + selected.id);
                if (!health_action) throw std::runtime_error("unexpected health callback");
                return health_action(project, selected);
            }};
    }
};

void strict_v2_schema_selector_and_budget_limits() {
    const Json valid_members = Json::array({member("alpha", "route", "alpha-route", "alpha query", true)});
    const std::vector<Json> invalid = {
        nullptr,
        Json::array(),
        {{"task_id", "task"}, {"members", valid_members}, {"max_evidence_bytes", 1024}},
        request(valid_members, 1024, "task", "shin.session-bootstrap-request.v1"),
        request(valid_members, 1024, "task", nullptr),
        {{"schema_version", "shin.session-bootstrap-request.v2"}, {"task_id", "task"}, {"members", valid_members},
         {"max_evidence_bytes", 1024}, {"extra", true}},
        request(valid_members, 1023),
        request(valid_members, 131073),
        request(valid_members, 1024.5),
        request(valid_members, true),
        request(Json::array(), 1024),
        request(Json::array({member("alpha", "route", "r1", "q", true), member("bravo", "route", "r2", "q", false),
                             member("charlie", "route", "r3", "q", false), member("delta", "route", "r4", "q", false),
                             member("echo", "route", "r5", "q", false), member("foxtrot", "route", "r6", "q", false),
                             member("golf", "route", "r7", "q", false), member("hotel", "route", "r8", "q", false),
                             member("india", "route", "r9", "q", false)}), 1024),
        request(Json::array({member("alpha", "route", "alpha-route", "query", false)}), 1024),
        request(Json::array({member("alpha", "route", "same", "query", true),
                             member("alpha", "route", "same", "another", false)}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"route_id", "legacy-route"}, {"query", "query"},
                             {"required", true}}}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", selector("route", "route-id")},
                             {"route_id", "ambiguous-route"}, {"query", "query"}, {"required", true}}}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", {{"kind", "route"}, {"id", "route-id"},
                                                                            {"publication_id", "shared"}}},
                             {"query", "query"}, {"required", true}}}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", nullptr}, {"query", "query"}, {"required", true}}}),
                1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", {{"kind", nullptr}, {"id", "route-id"}}},
                             {"query", "query"}, {"required", true}}}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", {{"kind", "route"}, {"id", nullptr}}},
                             {"query", "query"}, {"required", true}}}), 1024),
        request(Json::array({member("alpha", "unknown", "route-id", "query", true)}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", selector("route", "route-id")},
                             {"query", nullptr}, {"required", true}}}), 1024),
        request(Json::array({{{"project_id", "alpha"}, {"selector", selector("route", "route-id")},
                             {"query", "query"}, {"required", nullptr}}}), 1024),
        request(Json::array({member("alpha", "route", "route-id", std::string(2049, 'q'), true)}), 1024),
        request(valid_members, 1024, "invalid task"),
        request(valid_members, 1024, std::string(129, 'a')),
    };

    for (const auto& value : invalid) {
        AccessProbe probe;
        expect_error(400, "invalid-request", [&] { (void)session_bootstrap(value, probe.access()); });
        check(probe.authorize_calls == 0 && probe.retrieve_calls == 0 && probe.health_calls == 0 && probe.calls.empty(),
              "malformed v2 bootstrap request invoked an access callback");
    }

    const auto route_context = brief("alpha", "shared", 1, 0, Json::array({{{"fact", "route"}}}));
    const auto publication_context = brief("alpha", "source-route", 7, 0, Json::array({{{"fact", "publication"}}}), "shared");
    AccessProbe accepted;
    accepted.retrieve_action = [&](const std::string&, const BootstrapSelector& selected, const std::string&) {
        return selected.kind == BootstrapSelectorKind::route ? route_context : publication_context;
    };
    accepted.health_action = [&](const std::string&, const BootstrapSelector& selected) {
        return health(selected.kind == BootstrapSelectorKind::route ? route_context : publication_context);
    };
    const auto result = session_bootstrap(request(Json::array({
        member("alpha", "route", "shared", "route", true), member("alpha", "publication", "shared", "publication", false),
    })), accepted.access());
    check(result.at("schema_version") == "shin.session-bootstrap.v2" && result.at("members").size() == 2 &&
              result.at("members").at(0).at("selector") == selector("route", "shared") &&
              result.at("members").at(1).at("selector") == selector("publication", "shared") &&
              accepted.authorize_calls == 2 && accepted.retrieve_calls == 2 && accepted.health_calls == 2,
          "route and publication selectors with the same identifier were not retained as distinct v2 members");

    const auto eight = Json::array({
        member("alpha", "route", "r1", "q", true), member("bravo", "route", "r2", "q", false),
        member("charlie", "route", "r3", "q", false), member("delta", "route", "r4", "q", false),
        member("echo", "route", "r5", "q", false), member("foxtrot", "route", "r6", "q", false),
        member("golf", "route", "r7", "q", false), member("hotel", "route", "r8", "q", false),
    });
    AccessProbe boundary;
    boundary.retrieve_action = [](const std::string& project, const BootstrapSelector& selected, const std::string&) {
        return brief(project, selected.id, 1, 0, Json::array({{{"fact", "one"}}}));
    };
    boundary.health_action = [](const std::string& project, const BootstrapSelector& selected) {
        return health(brief(project, selected.id, 1, 0, Json::array({{{"fact", "one"}}})));
    };
    const auto boundary_result = session_bootstrap(request(eight), boundary.access());
    check(boundary_result.at("members").size() == 8 && boundary.authorize_calls == 8 && boundary.retrieve_calls == 8 &&
              boundary.health_calls == 8 && boundary_result.at("members").at(0).at("context") ==
                  brief("alpha", "r1", 1, 0, Json::array({{{"fact", "one"}}})),
          "valid v2 bootstrap boundary request did not retain all eight members");
}

void authorizes_every_member_before_retrieval_and_orders_required_first() {
    AccessProbe denied;
    denied.authorize_action = [](const std::string& project) {
        if (project == "denied") throw ServiceError(403, "forbidden", "optional project is forbidden");
    };
    const auto input = request(Json::array({
        member("optional-one", "route", "route-o1", "one", false),
        member("required-one", "route", "route-r1", "two", true),
        member("denied", "publication", "route-o2", "three", false),
        member("required-two", "publication", "route-r2", "four", true),
    }));
    expect_error(403, "forbidden", [&] { (void)session_bootstrap(input, denied.access()); });
    check(denied.authorize_calls == 4 && denied.retrieve_calls == 0 && denied.health_calls == 0 &&
              denied.calls == std::vector<std::string>({"authorize:required-one", "authorize:required-two",
                  "authorize:optional-one", "authorize:denied"}),
          "bootstrap did not authorize every project before selector lookup or retrieval");

    AccessProbe ordered;
    ordered.retrieve_action = [](const std::string& project, const BootstrapSelector& selected, const std::string&) {
        return brief(project, selected.kind == BootstrapSelectorKind::route ? selected.id : "publication-source", 9, 0,
                     Json::array({{{"fact", project}}}),
                     selected.kind == BootstrapSelectorKind::publication ? selected.id : "");
    };
    ordered.health_action = [](const std::string& project, const BootstrapSelector& selected) {
        return health(brief(project, selected.kind == BootstrapSelectorKind::route ? selected.id : "publication-source", 9, 0,
                            Json::array({{{"fact", project}}}),
                            selected.kind == BootstrapSelectorKind::publication ? selected.id : ""));
    };
    const auto result = session_bootstrap(request(Json::array({
        member("optional-one", "route", "route-o1", "one", false),
        member("required-one", "route", "route-r1", "two", true),
        member("optional-two", "publication", "shared-o2", "three", false),
        member("required-two", "publication", "shared-r2", "four", true),
    })), ordered.access());
    const auto& results = result.at("members");
    const auto retained_bytes = results.at(0).at("context").dump().size() + results.at(1).at("context").dump().size() +
                                results.at(2).at("context").dump().size() + results.at(3).at("context").dump().size();
    check(result.at("task_id") == "session-bootstrap-test" && result.at("ready") == true &&
              result.at("context_ready") == true && result.at("health_ready") == true && result.at("read_only") == true &&
              result.at("snapshot_consistency") == "independently-observed-members" && result.at("checkpoint_restored") == false &&
              result.at("max_evidence_bytes") == 4096 && result.at("evidence_bytes") == retained_bytes &&
              ordered.authorize_calls == 4 && ordered.retrieve_calls == 4 && ordered.health_calls == 4 &&
              ordered.calls == std::vector<std::string>({
                  "authorize:required-one", "authorize:required-two", "authorize:optional-one", "authorize:optional-two",
                  "retrieve:required-one/route/route-r1/two", "health:required-one/route/route-r1",
                  "retrieve:required-two/publication/shared-r2/four", "health:required-two/publication/shared-r2",
                  "retrieve:optional-one/route/route-o1/one", "health:optional-one/route/route-o1",
                  "retrieve:optional-two/publication/shared-o2/three", "health:optional-two/publication/shared-o2"}) &&
              results.at(0).at("project_id") == "required-one" && results.at(0).at("selector") == selector("route", "route-r1") &&
              results.at(1).at("project_id") == "required-two" &&
              results.at(1).at("selector") == selector("publication", "shared-r2") &&
              results.at(2).at("project_id") == "optional-one" && results.at(3).at("project_id") == "optional-two",
          "bootstrap did not retain required-first kind-aware member ordering");
}

void retains_route_and_publication_source_provenance() {
    const auto route_context = brief("alpha", "private-route", 13, 0, Json::array({{{"fact", "route"}}}));
    const auto publication_context = brief("bravo", "published-source-route", 41, 0,
                                           Json::array({{{"fact", "publication"}}}), "shared-publication");
    AccessProbe probe;
    probe.retrieve_action = [&](const std::string&, const BootstrapSelector& selected, const std::string&) {
        return selected.kind == BootstrapSelectorKind::route ? route_context : publication_context;
    };
    probe.health_action = [&](const std::string&, const BootstrapSelector& selected) {
        return health(selected.kind == BootstrapSelectorKind::route ? route_context : publication_context);
    };
    const auto result = session_bootstrap(request(Json::array({
        member("alpha", "route", "private-route", "route-query", true),
        member("bravo", "publication", "shared-publication", "publication-query", true),
    })), probe.access());
    const auto& publication = result.at("members").at(1);
    check(result.at("ready") == true && publication.at("selector") == selector("publication", "shared-publication") &&
              !publication.contains("route_id") && publication.at("context") == publication_context &&
              publication.at("context").at("route_id") == "published-source-route" &&
              publication.at("context").at("route_id") != publication.at("selector").at("id") &&
              publication.at("context").at("publication_id") == "shared-publication" &&
              probe.calls == std::vector<std::string>({"authorize:alpha", "authorize:bravo",
                  "retrieve:alpha/route/private-route/route-query", "health:alpha/route/private-route",
                  "retrieve:bravo/publication/shared-publication/publication-query", "health:bravo/publication/shared-publication"}),
          "publication selection did not preserve its source route provenance or dispatch through the publication selector");
}

void required_optional_and_evidence_states() {
    AccessProbe probe;
    probe.retrieve_action = [](const std::string& project, const BootstrapSelector& selected, const std::string&) {
        if (project == "optional-missing" || project == "required-missing")
            throw ServiceError(503, "active-generation-missing", "fixture missing route");
        if (project == "partial") return brief(project, selected.id, 3, 1, Json::array({{{"fact", "partial"}}}));
        return brief(project, selected.id, 3, 0, Json::array({{{"fact", "complete"}}}));
    };
    probe.health_action = [](const std::string& project, const BootstrapSelector& selected) {
        return health(brief(project, selected.id, 3, 0, Json::array({{{"fact", "health"}}})));
    };
    const auto optional = session_bootstrap(request(Json::array({
        member("optional-missing", "route", "missing-route", "missing", false),
        member("required-ready", "route", "ready-route", "ready", true),
    })), probe.access());
    check(optional.at("ready") == true && optional.at("context_ready") == true && optional.at("health_ready") == true &&
              optional.at("members").at(0).at("project_id") == "required-ready" &&
              optional.at("members").at(0).at("state") == "ready" && optional.at("members").at(1).at("state") == "unavailable" &&
              optional.at("members").at(1).at("reason_code") == "active-generation-missing" &&
              probe.authorize_calls == 2 && probe.retrieve_calls == 2 && probe.health_calls == 2,
          "optional missing route changed required readiness or callback coverage");

    AccessProbe required_probe;
    required_probe.retrieve_action = probe.retrieve_action;
    required_probe.health_action = probe.health_action;
    const auto required = session_bootstrap(request(Json::array({
        member("required-missing", "route", "missing-route", "missing", true),
        member("optional-ready", "route", "ready-route", "ready", false),
    })), required_probe.access());
    check(required.at("ready") == false && required.at("context_ready") == false && required.at("health_ready") == true &&
              required.at("members").at(0).at("state") == "unavailable" &&
              required.at("members").at(0).at("reason_code") == "active-generation-missing" &&
              required_probe.authorize_calls == 2 && required_probe.retrieve_calls == 2 && required_probe.health_calls == 2,
          "required missing route did not disable bootstrap readiness");

    AccessProbe states;
    states.retrieve_action = [](const std::string& project, const BootstrapSelector& selected, const std::string&) {
        if (project == "partial") return brief(project, selected.id, 4, 1, Json::array({{{"fact", "partial"}}}));
        return brief(project, selected.id, 4, 0, Json::array());
    };
    states.health_action = [](const std::string& project, const BootstrapSelector& selected) {
        return health(brief(project, selected.id, 4, 0, Json::array({{{"fact", "health"}}})));
    };
    const auto result = session_bootstrap(request(Json::array({
        member("partial", "route", "partial-route", "partial", true),
        member("empty", "route", "empty-route", "empty", false),
    })), states.access());
    const auto& partial = result.at("members").at(0);
    const auto& empty = result.at("members").at(1);
    check(result.at("ready") == false && result.at("context_ready") == false && result.at("health_ready") == true &&
              partial.at("state") == "partial-coverage" && partial.at("coverage_complete") == false &&
              partial.at("context").at("evidence").at("query_coverage").at("uncovered_group_count") == 1 &&
              empty.at("state") == "no-evidence" && empty.at("coverage_complete") == true && empty.at("context").is_object() &&
              states.authorize_calls == 2 && states.retrieve_calls == 2 && states.health_calls == 2,
          "partial coverage or empty evidence state changed");
}

void accounts_whole_context_dump_without_partial_mutation() {
    std::string utf8;
    for (int index = 0; index < 280; ++index) utf8.append("\xC3\xA9");
    const auto first = brief("alpha", "alpha-route", 17, 0, Json::array({{{"fact", utf8}, {"score", 1.25}, {"rank", 2.5}}}));
    const auto second = brief("bravo", "bravo-route", 18, 0, Json::array({{{"fact", utf8}, {"score", 3.75}, {"rank", 4.5}}}));
    check(first.dump().size() <= 1024 && second.dump().size() <= 1024 && first.dump().size() + second.dump().size() > 1024,
          "UTF-8 floating-point context fixture does not straddle the shared budget");

    AccessProbe probe;
    probe.retrieve_action = [&](const std::string& project, const BootstrapSelector&, const std::string&) {
        return project == "alpha" ? first : second;
    };
    probe.health_action = [&](const std::string& project, const BootstrapSelector&) {
        return health(project == "alpha" ? first : second);
    };
    const auto result = session_bootstrap(request(Json::array({
        member("alpha", "route", "alpha-route", "alpha", true), member("bravo", "route", "bravo-route", "bravo", true),
    }), 1024), probe.access());
    const auto& accepted = result.at("members").at(0);
    const auto& rejected = result.at("members").at(1);
    check(result.at("evidence_bytes") == first.dump().size() && result.at("max_evidence_bytes") == 1024 &&
              accepted.at("state") == "ready" && accepted.at("context") == first &&
              rejected.at("state") == "budget-exceeded" && rejected.at("reason_code") == "bootstrap-evidence-budget" &&
              rejected.at("context").is_null() && rejected.at("coverage_complete") == true &&
              result.at("context_ready") == false && result.at("health_ready") == true && result.at("ready") == false &&
              probe.authorize_calls == 2 && probe.retrieve_calls == 2 && probe.health_calls == 2,
          "bootstrap did not account for whole native JSON contexts without retaining a partial canonical envelope");
}

void rejects_context_binding_mismatches_and_publication_fallback() {
    const auto route_context = brief("wrong-project", "expected-route", 31, 0, Json::array({{{"fact", "wrong"}}}));
    AccessProbe route_mismatch;
    route_mismatch.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) { return route_context; };
    route_mismatch.health_action = [&](const std::string&, const BootstrapSelector&) { return health(route_context); };
    const auto route_result = session_bootstrap(request(Json::array({
        member("alpha", "route", "expected-route", "query", true),
    })), route_mismatch.access());
    check(route_result.at("context_ready") == false && route_result.at("health_ready") == true &&
              route_result.at("members").at(0).at("state") == "unavailable" &&
              route_result.at("members").at(0).at("reason_code") == "bootstrap-context-binding-mismatch" &&
              route_result.at("members").at(0).at("context").is_null() && route_mismatch.retrieve_calls == 1 &&
              route_mismatch.health_calls == 1,
          "route context project binding mismatch was not surfaced per member");

    const auto wrong_route_context = brief("alpha", "wrong-route", 31, 0, Json::array({{{"fact", "wrong"}}}));
    AccessProbe route_id_mismatch;
    route_id_mismatch.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) {
        return wrong_route_context;
    };
    route_id_mismatch.health_action = [&](const std::string&, const BootstrapSelector&) { return health(wrong_route_context); };
    const auto route_id_result = session_bootstrap(request(Json::array({
        member("alpha", "route", "expected-route", "query", true),
    })), route_id_mismatch.access());
    check(route_id_result.at("context_ready") == false && route_id_result.at("members").at(0).at("state") == "unavailable" &&
              route_id_result.at("members").at(0).at("reason_code") == "bootstrap-context-binding-mismatch" &&
              route_id_mismatch.calls == std::vector<std::string>({"authorize:alpha",
                  "retrieve:alpha/route/expected-route/query", "health:alpha/route/expected-route"}),
          "route context binding did not require the exact route identifier");

    const auto publication_context = brief("alpha", "publication-source", 32, 0, Json::array({{{"fact", "wrong"}}}),
                                           "other-publication");
    AccessProbe publication_mismatch;
    publication_mismatch.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) {
        return publication_context;
    };
    publication_mismatch.health_action = [&](const std::string&, const BootstrapSelector&) { return health(publication_context); };
    const auto publication_result = session_bootstrap(request(Json::array({
        member("alpha", "publication", "selected-publication", "query", true),
    })), publication_mismatch.access());
    check(publication_result.at("context_ready") == false && publication_result.at("members").at(0).at("state") == "unavailable" &&
              publication_result.at("members").at(0).at("reason_code") == "bootstrap-context-binding-mismatch" &&
              publication_mismatch.calls == std::vector<std::string>({"authorize:alpha",
                  "retrieve:alpha/publication/selected-publication/query", "health:alpha/publication/selected-publication"}),
          "publication context binding did not require the exact publication identifier");

    const auto non_current = [&] {
        auto context = brief("alpha", "expected-route", 33, 0, Json::array({{{"fact", "stale"}}}));
        context["admission"] = "snapshot-only";
        return context;
    }();
    AccessProbe admission_mismatch;
    admission_mismatch.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) { return non_current; };
    admission_mismatch.health_action = [&](const std::string&, const BootstrapSelector&) { return health(non_current); };
    const auto admission_result = session_bootstrap(request(Json::array({
        member("alpha", "route", "expected-route", "query", true),
    })), admission_mismatch.access());
    check(admission_result.at("members").at(0).at("reason_code") == "bootstrap-context-binding-mismatch" &&
              admission_result.at("members").at(0).at("context").is_null() && admission_mismatch.retrieve_calls == 1 &&
              admission_mismatch.health_calls == 1,
          "bootstrap accepted a non-current context admission");

    AccessProbe withdrawn;
    withdrawn.retrieve_action = [](const std::string&, const BootstrapSelector& selected, const std::string&) -> Json {
        check(selected.kind == BootstrapSelectorKind::publication && selected.id == "withdrawn-publication",
              "withdrawn fixture dispatched to a route selector");
        throw ServiceError(410, "publication-withdrawn", "fixture withdrawn publication");
    };
    const auto health_context = brief("alpha", "private-route", 34, 0, Json::array({{{"fact", "health"}}}),
                                      "withdrawn-publication");
    withdrawn.health_action = [&](const std::string&, const BootstrapSelector&) { return health(health_context); };
    const auto withdrawn_result = session_bootstrap(request(Json::array({
        member("alpha", "publication", "withdrawn-publication", "query", true),
    })), withdrawn.access());
    check(withdrawn_result.at("context_ready") == false && withdrawn_result.at("health_ready") == true &&
              withdrawn_result.at("members").at(0).at("reason_code") == "publication-withdrawn" &&
              withdrawn.retrieve_calls == 1 && withdrawn.health_calls == 1 &&
              withdrawn.calls == std::vector<std::string>({"authorize:alpha",
                  "retrieve:alpha/publication/withdrawn-publication/query", "health:alpha/publication/withdrawn-publication"}),
          "withdrawn publication retried or fell back to a route retrieval");
}

void separates_context_and_health_failures() {
    const auto context = brief("alpha", "alpha-route", 41, 0, Json::array({{{"fact", "ready"}}}));
    const auto publication_context = brief("alpha", "publication-source-route", 42, 0,
                                           Json::array({{{"fact", "publication-ready"}}}), "shared-publication");
    const std::vector<std::pair<std::string, Json>> selection_changes{
        {"project_id", "different-project"}, {"route_id", "different-route"},
        {"workspace_id", "different-workspace"}, {"source_host_id", "different-host"},
        {"snapshot_id", "different-snapshot"}, {"preparation_id", 102}, {"revision", 43},
        {"publication_id", "different-publication"},
    };
    for (const auto& [field, value] : selection_changes) {
        AccessProbe selection_changed;
        selection_changed.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) {
            return publication_context;
        };
        selection_changed.health_action = [&](const std::string&, const BootstrapSelector&) {
            auto observed = health(publication_context);
            observed["current_generation"][field] = value;
            return observed;
        };
        const auto mismatched = session_bootstrap(request(Json::array({
            member("alpha", "publication", "shared-publication", "query", true),
        })), selection_changed.access());
        const auto& mismatched_member = mismatched.at("members").at(0);
        check(mismatched.at("context_ready") == true && mismatched.at("health_ready") == false && mismatched.at("ready") == false &&
                  mismatched_member.at("state") == "ready" && mismatched_member.at("context") == publication_context &&
                  mismatched_member.at("health_ready") == false &&
                  mismatched_member.at("health_reason_code") == "bootstrap-selection-changed" &&
                  mismatched_member.at("health").at("current_generation").at(field) == value &&
                  selection_changed.authorize_calls == 1 && selection_changed.retrieve_calls == 1 && selection_changed.health_calls == 1 &&
                  selection_changed.calls == std::vector<std::string>({"authorize:alpha",
                      "retrieve:alpha/publication/shared-publication/query", "health:alpha/publication/shared-publication"}),
              "health generation disagreement did not preserve the publication context with bootstrap-selection-changed");
    }

    for (const char* state : {"stale", "expired"}) {
        AccessProbe unavailable_health;
        unavailable_health.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) { return context; };
        unavailable_health.health_action = [&](const std::string&, const BootstrapSelector&) {
            return health(context, true, state);
        };
        const auto unavailable = session_bootstrap(request(Json::array({
            member("alpha", "route", "alpha-route", "query", true),
        })), unavailable_health.access());
        const auto& unavailable_member = unavailable.at("members").at(0);
        check(unavailable.at("context_ready") == true && unavailable.at("health_ready") == false && unavailable.at("ready") == false &&
                  unavailable_member.at("state") == "ready" && unavailable_member.at("context") == context &&
                  unavailable_member.at("health").at("state") == state && unavailable_member.at("health_ready") == false &&
                  unavailable_member.at("health_reason_code").is_null() && unavailable_health.authorize_calls == 1 &&
                  unavailable_health.retrieve_calls == 1 && unavailable_health.health_calls == 1,
              "stale or expired assessment was promoted by an otherwise matching generation identity");
    }

    AccessProbe retrieval_failure;
    retrieval_failure.retrieve_action = [](const std::string&, const BootstrapSelector&, const std::string&) -> Json {
        throw ServiceError(409, "stale-index", "fixture retrieval failure");
    };
    retrieval_failure.health_action = [&](const std::string&, const BootstrapSelector&) { return health(context); };
    const auto retrieval_failed = session_bootstrap(request(Json::array({member("alpha", "route", "alpha-route", "query", true)})),
                                                    retrieval_failure.access());
    check(retrieval_failed.at("context_ready") == false && retrieval_failed.at("health_ready") == true &&
              retrieval_failed.at("members").at(0).at("state") == "unavailable" &&
              retrieval_failed.at("members").at(0).at("reason_code") == "stale-index" &&
              retrieval_failed.at("members").at(0).at("health").at("state") == "healthy" &&
              retrieval_failure.authorize_calls == 1 && retrieval_failure.retrieve_calls == 1 && retrieval_failure.health_calls == 1,
          "retrieval failure did not retain the independent health result");

    AccessProbe health_failure;
    health_failure.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) { return context; };
    health_failure.health_action = [](const std::string&, const BootstrapSelector&) -> Json {
        throw ServiceError(503, "knowledge-unavailable", "fixture health failure");
    };
    const auto health_failed = session_bootstrap(request(Json::array({member("alpha", "route", "alpha-route", "query", true)})),
                                                 health_failure.access());
    const auto& failed_member = health_failed.at("members").at(0);
    check(health_failed.at("context_ready") == true && health_failed.at("health_ready") == false && health_failed.at("ready") == false &&
              failed_member.at("state") == "ready" && failed_member.at("reason_code").is_null() &&
              failed_member.at("context") == context && failed_member.at("health").is_null() &&
              failed_member.at("health_ready") == false && failed_member.at("health_reason_code") == "knowledge-unavailable" &&
              health_failure.authorize_calls == 1 && health_failure.retrieve_calls == 1 && health_failure.health_calls == 1,
          "health failure did not preserve context and its separate reason code");
}

void rejects_oversized_response() {
    const auto context = brief("alpha", "alpha-route", 51, 0, Json::array({{{"fact", "small"}}}));
    AccessProbe probe;
    probe.retrieve_action = [&](const std::string&, const BootstrapSelector&, const std::string&) { return context; };
    probe.health_action = [&](const std::string&, const BootstrapSelector&) {
        auto observed = health(context);
        observed["fixture_payload"] = std::string(270000, 'x');
        return observed;
    };
    expect_error(413, "bootstrap-response-too-large", [&] {
        (void)session_bootstrap(request(Json::array({member("alpha", "route", "alpha-route", "query", true)})), probe.access());
    });
    check(probe.authorize_calls == 1 && probe.retrieve_calls == 1 && probe.health_calls == 1 &&
              probe.calls == std::vector<std::string>({"authorize:alpha", "retrieve:alpha/route/alpha-route/query",
                  "health:alpha/route/alpha-route"}),
          "oversized bootstrap response was not bounded after the complete controlled callback sequence");
}

} // namespace

int main() {
    try {
        strict_v2_schema_selector_and_budget_limits();
        authorizes_every_member_before_retrieval_and_orders_required_first();
        retains_route_and_publication_source_provenance();
        required_optional_and_evidence_states();
        accounts_whole_context_dump_without_partial_mutation();
        rejects_context_binding_mismatches_and_publication_fallback();
        separates_context_and_health_failures();
        rejects_oversized_response();
        std::cout << "session bootstrap v2 authorization, selector, readiness, budget and output-bound tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
