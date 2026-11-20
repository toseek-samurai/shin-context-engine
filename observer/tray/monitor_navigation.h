#pragma once

#include "project_disconnection.h"

std::string selected;
int dashboard_view = 0, history_hours = 1, project_page = 0, model_page = 0;
std::string model_project;
enum MonitorAction : int {
    about = 14, back_from_utility = 160, project_settings = 161, project_exclusions = 162,
    confirm_disconnect = 163, disconnect_project = 164, project_journal = 165,
    copy_version_details = 166, open_release_notes = 167, open_repository = 168, open_licenses = 169,
    back_to_project = 170, back_to_project_settings = 171, open_exclusion_guide = 172,
    open_homebrew_guide = 173, journal_back = 174, compact_view = 175,
    update_settings = 176, history_settings = 177, back_to_settings = 178, back_from_journal_entry = 179,
    open_project_list = 180, choose_language = 181, journal_session_details = 182, open_journal_record = 183
};
enum class ProjectPage { overview, settings, exclusions, disconnect };
ProjectPage project_view = ProjectPage::overview;
enum class SettingsPage { overview, updates, history };
SettingsPage settings_view = SettingsPage::overview;
int utility_return_view = 0;
std::string journal_project, version_details;
struct JournalEntry {
    std::string session, project, client;
    shin::ui::SessionActivityEvent event;
};
std::array<std::optional<JournalEntry>, shin::ui::session_activity_max_journal_events> journal_rows;
std::optional<JournalEntry> journal_entry;
bool version_details_copied = false;
std::string monitor_link(int code) {
    const std::string repository = "https://github.com/toseek-samurai/shin-context-engine";
    switch (code) {
    case open_release_notes: return repository + "/releases/tag/v" + SHIN_OBSERVER_VERSION;
    case open_licenses: return repository + "/blob/main/THIRD_PARTY.md";
    case open_exclusion_guide: return repository + "/blob/main/docs/local-kit.md#project-text-and-exclusions";
    case open_homebrew_guide: return repository + "/blob/main/docs/homebrew.md#upgrade";
    default: return repository;
    }
}
bool diagnostic_history_first = false;
bool diagnostic_details_open = false;
int activity_view = 2;
bool timing_open = false, timing_retained = false;
bool session_notes_open = false;
int session_content_mode = -1;
std::string session_details_open;
int timing_filter = 0;
std::string diagnostic_project, diagnostic_name, diagnostic_copy, diagnostic_root, history_project, diagnostic_report_text;
std::atomic<bool> project_management_busy{false};
bool profile_review_open = false;
std::string profile_review_root;
std::shared_ptr<ProfileReviewChannel> active_profile_review;
Clock::time_point diagnostic_copied{};

void select_project(const std::string& id) {
    selected = selected == id ? "" : id;
    diagnostic_history_first = false;
    diagnostic_details_open = false;
    diagnostic_copied = {};
    profile_review_open = false;
    project_view = ProjectPage::overview;
}

void activate(int code, const std::string& target = {}) {
    if (code == -1 && dashboard_view == 1 && !target.empty()) select_project(target);
    else if (code == open_project_list) activate(11);
    else if (code >= 10 && code <= about) {
        if (dashboard_view <= 1) utility_return_view = dashboard_view;
        dashboard_view = code - 10;
        if (code == 11) { selected.clear(); project_view = ProjectPage::overview; diagnostic_history_first = diagnostic_details_open = false; }
        if (code == 12) { journal_project.clear(); journal_entry.reset(); activity_view = 2; timing_open = false; }
        if (code == 13) settings_view = SettingsPage::overview;
        if (code == about) version_details_copied = false;
        platform_save_view(std::min(dashboard_view, 3));
    } else if (code == back_from_utility || code == journal_back) {
        dashboard_view = utility_return_view;
        platform_save_view(dashboard_view);
    } else if (code == update_settings || code == history_settings || code == back_to_settings) {
        settings_view = code == update_settings ? SettingsPage::updates :
            code == history_settings ? SettingsPage::history : SettingsPage::overview;
    } else if (code == project_settings || code == back_to_project_settings) {
        project_view = ProjectPage::settings;
    } else if (code == project_exclusions) {
        project_view = ProjectPage::exclusions;
    } else if (code == confirm_disconnect) {
        project_view = ProjectPage::disconnect;
    } else if (code == back_to_project) {
        project_view = ProjectPage::overview;
    } else if (code == disconnect_project && dashboard_view == 1 && project_view == ProjectPage::disconnect &&
        selected == diagnostic_project && !project_management_busy.load()) {
        if (const auto request = project_disconnection.begin(selected, diagnostic_root, diagnostic_name))
            platform_disconnect(*request);
    } else if (code == project_journal && !selected.empty()) {
        activate(12);
        journal_project = selected;
    } else if (code == back_from_journal_entry) {
        journal_entry.reset();
    } else if (code == open_journal_record && dashboard_view == 2 && activity_view == 2 && !journal_entry && !target.empty()) {
        const auto record = std::find_if(journal_rows.begin(), journal_rows.end(), [&](const auto& row) {
            return row && row->event.key == target;
        });
        if (record != journal_rows.end()) journal_entry = *record;
    } else if (code == journal_session_details && dashboard_view == 2 && journal_entry) {
        const auto& id = journal_entry->session;
        session_details_open = session_details_open == id ? "" : id;
    } else if (code == copy_version_details) {
        version_details_copied = platform_copy(version_details);
    } else if (code == open_release_notes || code == open_repository || code == open_licenses ||
        code == open_exclusion_guide || code == open_homebrew_guide || code == compact_view || code == choose_language) {
        platform_action(code);
    } else if (code >= 20 && code <= 22) history_hours = code == 20 ? 0 : code == 21 ? 24 : 168;
    else if (code == 23) history_hours = 1;
    else if (code == 30) project_page = std::max(0, project_page - 1);
    else if (code == 31) ++project_page;
    else if (code == 32) { selected.clear(); diagnostic_history_first = false; project_view = ProjectPage::overview; }
    else if (code == 34) diagnostic_history_first = true;
    else if (code == 35) diagnostic_history_first = false;
    else if (code == 36 && selected == diagnostic_project && !diagnostic_copy.empty()) {
        if (platform_copy(diagnostic_copy)) diagnostic_copied = Clock::now();
    } else if (code == 37 && !history_project.empty()) {
        activate(11);
        selected = history_project; diagnostic_history_first = true; diagnostic_copied = {};
    } else if (code == 38 && selected == diagnostic_project && !diagnostic_root.empty()) {
        platform_reveal(diagnostic_root);
    }
    else if (code == 70 && active_profile_review && !profile_review_root.empty())
        profile_review_open = active_profile_review->preview(profile_review_root);
    else if (code == 71 && active_profile_review) active_profile_review->accept();
    else if (code == 72) profile_review_open = false;
    else if (code == 73 && active_profile_review && !profile_review_root.empty())
        active_profile_review->preview(profile_review_root);
    else if (code == 41) model_page = std::max(0, model_page - 1);
    else if (code == 42) ++model_page;
    else if (code == 60) { activate(12); timing_open = true; timing_retained = history_hours != 0; activity_view = 0; }
    else if (code == 61) { timing_open = false; activity_view = 2; }
    else if (code >= 100 && code <= 102) { activity_view = code - 100; timing_open = activity_view == 0; }
    else if (code >= 113 && code <= 115) select_language_preference(code - 113);
    else if (code == 120) diagnostic_details_open = !diagnostic_details_open;
    else if (code == 121 && dashboard_view == 0 && !target.empty()) { activate(11); select_project(target); }
    else if (code >= 63 && code <= 65) timing_filter = code - 63;
    else if (code == 67 || code == 68) timing_retained = code == 68;
    else if (code == 90) session_notes_open = !session_notes_open;
    else if (code == 80) {
        bool expected = false;
        if (project_management_busy.compare_exchange_strong(expected, true)) platform_action(code);
    } else if ((code == 39 && selected == diagnostic_project && !diagnostic_report_text.empty()) ||
        code == 40 || code == 43 || code == 44 || code == 62 || code == 66 || code == 83 || code == 84 || code == 85 || code == 86 || (code >= 110 && code <= 112) || code == 116) platform_action(code);
}

void complete_panel_disconnect(std::uint64_t ticket, const Json& result, std::string failure = {}) {
    const auto* reply = &result;
    const auto service = result.is_object() ? result.find("service") : result.end();
    if (service != result.end() && (!service->is_object() || service->value("ok", Json()) != true ||
        service->value("error", Json()).is_object())) reply = &*service;
    const bool disconnected = failure.empty() && reply == &result && result.is_object() && result.value("ok", Json()) == true;
    if (failure.empty() && !disconnected && reply->is_object()) {
        const auto error = reply->value("error", Json());
        if (error.is_object()) failure = shin::observer::validation_identifier(error.value("code", Json()));
    }
    const auto* outcome = project_disconnection.complete(ticket, disconnected, std::move(failure));
    if (outcome && outcome->disconnected && dashboard_view == 1 && project_view == ProjectPage::disconnect &&
        selected == outcome->request.project) activate(32);
}

struct PanelAction { int code; std::string target; };
std::optional<PanelAction> pending_panel_action;

// ImGui emits input while building a frame. The frame uses its original route
// and record until all of its draw commands and accessible items are assembled.
void queue_panel_action(int code, const std::string& target = {}) {
    pending_panel_action = PanelAction{code, target};
}

void apply_panel_action() {
    const auto action = std::exchange(pending_panel_action, std::nullopt);
    if (action) activate(action->code, action->target);
}

bool back_from_panel_page() {
    if (dashboard_view == 1 && project_view != ProjectPage::overview) {
        activate(project_view == ProjectPage::settings ? back_to_project : back_to_project_settings);
    } else if (dashboard_view == 1 && diagnostic_details_open) {
        activate(120);
    } else if (dashboard_view == 2 && journal_entry) {
        activate(back_from_journal_entry);
    } else if (dashboard_view == 2 && timing_open) {
        activate(61);
    } else if (dashboard_view == 3 && settings_view != SettingsPage::overview) {
        activate(back_to_settings);
    } else if (dashboard_view >= 2) {
        activate(back_from_utility);
    } else return false;
    return true;
}
