#include "../ui/project_diagnostics.h"
#include "../ui/model_projects.h"
#include "monitor_motion.h"
#include <cassert>
#include <iostream>
#include <limits>

int check_language() try {
    using namespace shin::ui;
    MonitorTween tween;
    tween.snap(0);
    const float first = tween.step(1, .04f, false);
    assert(first > 0 && first < 1 && tween.moving());
    for (int i = 0; i < 4; ++i) tween.step(1, .04f, false);
    assert(tween.value() == 1 && !tween.moving());
    assert(tween.step(0, .01f, false) < 1);
    assert(tween.step(1, .01f, true) == 1 && !tween.moving());
    assert(tween.step(0, 2, false) == 0 && !tween.moving()); // Hidden-window resume snaps.
    const auto expected = [](const char* en, const char* ru) { return shin::ui::language() == shin::ui::Language::en ? en : ru; };
    const auto coverage = model_usage_coverage({{"status", "available"}, {"fresh", true}, {"complete", false},
        {"unassigned_sessions", 42}, {"skipped_sessions", 93}, {"sources", {{"codex", {{"state", "partial"},
        {"skipped_reasons", {{"rollout-missing", 92}, {"rollout-too-large", 1}}}}}}}});
    assert(coverage.find(expected("History file not found: 92", "Файл истории не найден: 92")) != std::string::npos);
    assert(coverage.find(expected("History file exceeds 4 GiB: 1", "Файл истории больше 4 ГиБ: 1")) != std::string::npos);
    assert(coverage.find("partial") == std::string::npos);
    assert(coverage.find("CMD") == std::string::npos);
    assert(model_usage_coverage({{"fresh", false}}).find(expected("stale", "устарела")) != std::string::npos);
    assert(std::string(usage_skip_reason("usage-source-unavailable")).find(expected("previous collector", "прежним сборщиком")) != std::string::npos);
    const Json usage{{"projects", Json::array({{{"id", ""}, {"name", "All"}},
        {{"id", "custom"}, {"name", "Group"}, {"workspace_keys", Json::array({"a", "b"})}}})}};
    const Json enrolled = Json::array({{{"id", "a"}, {"name", "First"}},
        {{"id", "b"}, {"name", "Second"}}, {{"id", "c"}, {"name", "Group"}}});
    const auto choices = model_project_choices(usage, enrolled);
    const auto labels = project_display_labels(usage, enrolled);
    assert(labels.at(0).at("name") == "Group / First");
    assert(labels.at(1).at("name") == "Group / Second");
    assert(labels.at(2) == enrolled.at(2));
    const auto single = project_display_labels(usage, Json::array({enrolled.at(0)}));
    assert(single.at(0).at("name") == "Group" && single.at(0).at("id") == "a");
    assert(project_display_labels(Json::object(), enrolled) == enrolled);
    assert(choices.size() == 3 && choices.at(0).at("key") == "");
    assert(choices.at(0).at("name") == shin::ui::tr("All usage projects"));
    assert(mapped_model_projects(usage, enrolled) == 2);
    assert(model_project_choices(Json::object(), enrolled).size() == 3);
    assert(mapped_model_projects(Json::object(), enrolled) == 0);
    assert(model_project_choices(usage, Json::array()).size() == 2);
    bool missing = false;
    for (const auto& item : choices) if (item.at("key") == "workspace/c") {
        missing = true; assert(item.at("usage_id").is_null() && item.at("configured") == false);
    }
    assert(missing); // Same name is not authority to borrow another project's usage.
    assert(project_table_layout(296, 9).page_size == 9);
    assert(project_table_layout(316, 9).page_size == 9);
    assert(project_table_layout(316, 9).pages == 1);
    assert(project_table_layout(296, 16).pages == 2);
    assert(project_table_layout(296, 1).table_height == project_table_layout(296, 16).table_height);
    assert(project_table_layout(0, 0).page_size == 1);
    const auto detailed = project_table_layout(296, 16, 44);
    assert(detailed.page_size == 5 && detailed.pages == 4);
    assert(detailed.table_height + ProjectTableLayout::controls_height <= 296);
    assert(project_table_layout(std::numeric_limits<float>::quiet_NaN(), 16).page_size == 1);
    assert(centered_item_y(24, 17) == 3);
    assert(centered_item_y(24, 13) == 5);
    assert(std::abs((centered_item_y(24, 17) + 8.5f) - (centered_item_y(24, 13) + 6.5f)) < 0.01f);
    const Json residents = Json::array({{{"root", "/a"}, {"rss_bytes", 10485760}}});
    const auto memory = resident_memory(residents, "/a", true);
    assert(memory && *memory == 10);
    assert(!resident_memory(residents, "/a", false));
    assert(!resident_memory(residents, "/b", true));
    assert(!resident_memory(Json::array({{{"root", "/a"}, {"rss_bytes", nullptr}}}), "/a", true));
    assert(!resident_memory(Json::array({{{"root", "/a"}, {"rss_bytes", -1}}}), "/a", true));
    Json project{{"id", "project-id"}, {"name", "project"}, {"root", "/tmp/project"},
        {"phase", "checking"}, {"last_completed_phase", "current"}, {"last_success_age_ms", 3000},
        {"activity_age_ms", 400}, {"last_check_age_ms", 3000}, {"active_issue", false},
        {"last_error_code", "refresh-validation-failed"}, {"error_code", ""}, {"failures", 3},
        {"ticks", 12}, {"publications", 2}, {"preparations", 1}, {"last_tick_ms", 42},
        {"authority", "local-canonical"}};
    assert(project_state(project, true) == ProjectState::checking);
    assert(std::string(state_text(project_state(project, true))) == shin::ui::tr("Checking files"));
    assert(!active_issue(project));
    assert(recovered(project));
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Checking changes"));
    assert(diagnostic_overview(project, false).title == shin::ui::tr("Service offline"));
    assert(diagnostic_history_status(project, false) == shin::ui::tr("Currently offline"));
    assert(diagnostic_history_status(project, true) == shin::ui::tr("Checks recovered"));
    assert(!retained_project(project, Json{{"status", "ready"}, {"projects", Json::array()}}));
    const Json saved_history{{"status", "ready"}, {"projects", Json::array({{{"id", "project-id"}, {"requests", 12}}})}};
    assert(retained_project(project, saved_history)->at("requests") == 12);
    assert(!retained_project(project, Json{{"status", "unavailable"}, {"projects", saved_history.at("projects")}}));
    assert(diagnostic_history_status(Json::object(), true) == shin::ui::tr("Previous check details unavailable"));
    const Json missing_history{{"telemetry", {{"history", {{"status", "ready"}, {"projects", Json::array()}}}}}};
    assert(Json::parse(diagnostic_summary(project, missing_history, true, "", "2.0.4")).at("retained_errors").is_null());
    assert(diagnostic_overview(Json{{"phase", "current"}, {"query_ready", false}, {"query_phase", "preparing"}}, true).title == shin::ui::tr("Preparing search"));
    project["query_ready"] = true;
    assert(project_state(project, true) == ProjectState::current);
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Context is current"));
    project["source_coverage"] = {{"state", "partial"}, {"quarantined_files", 1}};
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Context partially available"));
    assert(diagnostic_overview(project, false).title == shin::ui::tr("Service offline"));
    project.erase("source_coverage");
    project["query_ready"] = false;
    assert(project_state(project, true) == ProjectState::pending);
    project["query_phase"] = "standby";
    assert(project_state(project, true) == ProjectState::standby);
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Ready on demand"));
    project.erase("query_phase");
    project["active_issue"] = true;
    assert(project_state(project, true) == ProjectState::blocked);
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Check failed"));
    project["validation_diagnostic"] = {{"schema_version", "shin.validation-diagnostic.v1"},
        {"gates", Json::array({{{"id", "doctor"}, {"reasons", Json::array({"secret-like-source-content"})}}})}};
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Possible secret in sources"));
    assert(diagnostic_overview(project, true).summary.find(expected("Context unavailable", "Контекст недоступен")) != std::string::npos);
    assert(diagnostic_overview(project, false).title == shin::ui::tr("Service offline"));
    project["validation_diagnostic"] = {{"schema_version", "shin.validation-diagnostic.v1"},
        {"gates", Json::array({{{"id", "eval"}, {"reasons", Json::array({"query-failures"})},
            {"cases", Json::array({{{"id", "case-one"}, {"reasons", Json::array({"missing-required-path"})},
                {"expected_paths", Json::array({"docs/expected.md", "/private/path"})}}})}}})}};
    const auto missing_path_overview = diagnostic_overview(project, true);
    assert(missing_path_overview.title == shin::ui::tr("Search check failed"));
    assert(missing_path_overview.summary.find("docs/expected.md") != std::string::npos);
    assert(missing_path_overview.summary.find(expected("search results", "результатах поиска")) != std::string::npos);
    assert(missing_path_overview.summary.find(expected("missing on disk", "нет на диске")) != std::string::npos);
    assert(missing_path_overview.summary.find("/private/path") == std::string::npos);
    assert(missing_path_overview.next.find(expected("Shin will check the project again after changes", "После изменений Shin проверит проект снова")) != std::string::npos);
    assert(missing_path_overview.next.find(expected("If the file is present", "Если файл на месте")) != std::string::npos);
    assert(missing_path_overview.next.find(expected("save a report for Shin support", "сохраните отчёт для поддержки Shin")) != std::string::npos);
    assert(missing_path_overview.next.find(expected("check settings do not need changing", "менять настройки проверки не требуется")) != std::string::npos);
    assert(missing_path_overview.next.find(expected("fix the file", "исправьте файл")) == std::string::npos);
    project["active_issue"] = false;
    project["query_ready"] = true;
    project["query_phase"] = "ready";
    assert(diagnostic_overview(project, true).title == shin::ui::tr("Context is current"));
    project["active_issue"] = true;
    project["validation_diagnostic"]["gates"][0]["cases"] = Json::array();
    project["validation_diagnostic"]["gates"][0]["reasons"] = Json::array({"SENTINEL/private-path", "broken-local-link"});
    assert(diagnostic_overview(project, true).summary.find("broken-local-link") != std::string::npos);
    assert(diagnostic_overview(project, true).summary.find("SENTINEL") == std::string::npos);
    project.erase("validation_diagnostic");
    assert(diagnostic_history_status(project, true) == shin::ui::tr("Issue still active"));
    project["active_issue"] = false;
    project.erase("query_ready");
    project["recovering"] = true;
    assert(project_state(project, true) == ProjectState::recovering);
    assert(std::string(state_text(ProjectState::recovering)) == shin::ui::tr("Recovering index"));
    project["recovering"] = false;
    assert(project_state(project, false) == ProjectState::offline);
    project["last_completed_phase"] = "coalescing";
    assert(project_state(project, true) == ProjectState::pending);
    assert(!recovered(project)); // A successful check may predate the last failure.
    project["active_issue"] = true;
    assert(project_state(project, true) == ProjectState::blocked);
    assert(!recovered(project));
    project["phase"] = "checking";
    project["last_completed_phase"] = "blocked";
    project["error_code"] = "refresh-validation-failed";
    assert(project_state(project, true) == ProjectState::blocked);
    project["active_issue"] = false;
    project["last_completed_phase"] = "current";
    project["error_code"] = "";
    assert(project_state(project, true) == ProjectState::checking);
    assert(recovered(project));
    assert(count(project, "failures") == 3);
    project["last_completed_phase"] = "coalescing";
    project["phase"] = "blocked";
    project["active_issue"] = false;
    assert(project_state(project, true) != ProjectState::blocked);
    project.erase("active_issue");
    assert(active_issue(project));

    const Json legacy{{"phase", "checking"}, {"failures", 3}, {"error_code", "refresh-validation-failed"}};
    assert(!active_issue(legacy));
    assert(!recovered(legacy));
    assert(!age_ms(legacy, "last_success_age_ms"));
    assert(historical_code(legacy).empty());
    assert(age_text({}) == shin::ui::tr("unknown"));
    assert(age_text(999) == shin::ui::tr("just now"));
    assert(age_text(60000) == expected("1 min ago", "1 мин назад"));
    const auto age = age_ms(Json{{"age", 1000}}, "age", 1000);
    assert(age && *age == 2000);
    assert(!age_ms(Json{{"age", -1}}, "age"));
    assert(!age_ms(Json{{"age", nullptr}}, "age"));
    assert(!age_ms(Json{{"age", "1000"}}, "age"));
    assert(!age_ms(Json{{"age", std::numeric_limits<double>::infinity()}}, "age"));

    for (const auto* code : {"refresh-validation-failed", "writer-busy", "refresh-authority-changed",
                            "storage-full", "storage-read-only", "access-denied", "root-unavailable",
                            "storage-quota-exceeded", "storage-free-space-low",
                            "incomplete-source-observation", "project-check-failed",
                            "refresh-grant-invalid", "refresh-timeout", "service-timeout",
                            "service-disconnected", "stale-index", "refresh-superseded"}) {
        const auto explanation = explain(code);
        assert(!explanation.cause.empty());
        assert(!explanation.next.empty());
        assert(explanation.cause != explain("unknown-code-123").cause);
    }
    for (const auto* code : {"storage-quota-exceeded", "storage-free-space-low"}) {
        const auto explanation = explain(code);
        assert(explanation.next.find(expected("Storage", "Хранилище")) != std::string::npos);
        const Json refused{{"phase", "blocked"}, {"active_issue", true}, {"error_code", code}};
        const auto overview = diagnostic_overview(refused, true);
        assert(overview.next.find(expected("Storage", "Хранилище")) != std::string::npos);
        assert(overview.title != shin::ui::tr("Needs attention"));
    }
    const Json pending_profile{{"active_issue", false}, {"phase", "partial"}, {"query_ready", true},
        {"source_coverage", {{"state", "partial"}, {"reason", "profile-revision-pending"}}}};
    assert(project_state(pending_profile, true) == ProjectState::current);
    assert(project_state(pending_profile, false) == ProjectState::offline);
    assert(error_code(Json{{"code", "NEW_UNRECOGNIZED_CODE"}}, "code") == "NEW_UNRECOGNIZED_CODE");
    assert(error_code(Json{{"code", "token=secret\nsource body"}}, "code") == "unclassified");
    assert(error_code(Json{{"code", std::string(1000, 'x')}}, "code") == "unclassified");
    assert(text_field(Json{{"name", "a\nb\tc"}}, "name") == "a b c");
    const auto utf8 = text_field(Json{{"name", "Проверка"}}, "name", 3);
    assert(utf8 == "П…");

    project["active_issue"] = false;
    project["phase"] = "checking";
    project["prompt"] = "SENTINEL_PROMPT_SECRET";
    project["last_result"] = {{"error", {{"message", "SENTINEL_RAW_ERROR"}}}};
    project["token"] = "SENTINEL_TOKEN_SECRET";
    project["recoveries"] = 2;
    project["superseded_refreshes"] = 4;
    project["validation_diagnostic"] = {{"schema_version", "shin.validation-diagnostic.v1"},
        {"query", "SENTINEL_QUERY_SECRET"}, {"gates", Json::array({{{"id", "eval"},
            {"error_code", "file-changed-during-verification"}, {"message", "SENTINEL_GATE_SECRET"},
            {"reasons", Json::array({"index-not-current", "SENTINEL/path"})},
            {"cases", Json::array({{{"id", "case-one"}, {"source", "SENTINEL_SOURCE_SECRET"},
                {"reasons", Json::array({"missing-required-path"})}}})}}})}};
    Json service{{"version", "2.0-observer"}, {"core_version", "2.0-core"},
        {"telemetry", {{"captured_at_ms", 100000}, {"query", "SENTINEL_QUERY_SECRET"}}},
        {"source_body", "SENTINEL_SOURCE_SECRET"}};
    const auto summary = diagnostic_summary(project, service, false, "service-timeout", "0.4");
    const auto parsed = Json::parse(summary);
    assert(summary.size() < 4096);
    assert(summary.find("SENTINEL") == std::string::npos);
    assert(parsed.at("failures") == 3);
    assert(parsed.at("recoveries") == 2);
    assert(parsed.at("superseded_refreshes") == 4);
    assert(parsed.at("estimated_last_success_at_ms") == 97000);
    assert(parsed.at("captured_at_ms") == 100000);
    assert(parsed.at("last_completed_phase") == "coalescing");
    assert(parsed.at("connection_code") == "service-timeout");
    assert(parsed.at("last_error_code") == "refresh-validation-failed");
    assert(parsed.at("connected") == false);
    assert(parsed.at("validation_diagnostic").at("gates").at(0).at("cases").at(0).at("id") == "case-one");
    assert(parsed.at("validation_diagnostic").at("gates").at(0).at("error_code") == "file-changed-during-verification");
    assert(parsed.at("validation_diagnostic").at("gates").at(0).at("reasons") == Json::array({"index-not-current"}));
    project["name"] = std::string(10000, 'x');
    project["root"] = std::string(10000, 'x');
    assert(diagnostic_summary(project, service, true, "", "0.4").size() < 4096);
    assert(count(Json{{"failures", -1}}, "failures") == 0);
    assert(count(Json{}, "failures") == 0);
    assert(count(Json{{"failures", std::numeric_limits<std::uint64_t>::max()}}, "failures") == std::numeric_limits<std::uint64_t>::max());
    std::cout << "tray project diagnostics: passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "tray diagnostics: " << error.what() << '\n';
    return 1;
} catch (...) {
    return 1;
}

int main() {
    for (auto language : {shin::ui::Language::en, shin::ui::Language::ru}) {
        shin::ui::set_language(language); if (check_language()) return 1;
    }
}
