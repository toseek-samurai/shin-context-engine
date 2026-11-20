#pragma once

// Only expose visible content through accessibility; scrolled text must not
// create offscreen controls or cover the fixed footer.
void session_accessible(const std::string& label, const std::string& key, int action = 99) {
    const auto origin = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
    const auto window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    if (ImGui::IsItemVisible() && origin.x >= window.x && origin.y >= window.y &&
        end.x <= window.x + size.x && end.y <= window.y + size.y)
        ui_actions.push_back({label, key, origin, {end.x-origin.x,end.y-origin.y}, action, true, action == 99 || action == 87});
}

void session_text(const std::string& text, const std::string& key, float size = 14, ImVec4 color = ink, ImFont* font = nullptr) {
    ImGui::PushFont(font,size);
    ImGui::PushStyleColor(ImGuiCol_Text,color);
    ImGui::TextWrapped("%s",text.c_str());
    session_accessible(text,key);
    ImGui::PopStyleColor(); ImGui::PopFont();
}

std::string journal_name(const Json& projects, const std::string& id) {
    for (const auto& project : projects)
        if (project.value("id", "") == id) return shin::ui::text_field(project, "name");
    return id.empty() ? "" : shin::ui::tr("Project no longer listed");
}

std::string journal_operation(const shin::ui::SessionActivityEvent& event) {
    using shin::ui::tr;
    if (event.method == "query") return tr("Search");
    if (event.method == "read") return tr("File read");
    if (event.method == "inspect") return tr("Inspect");
    if (event.method == "resume") return tr("Resume");
    return event.label.empty() ? tr("Recorded MCP call") : event.label;
}

const char* journal_outcome(shin::ui::SessionEventTone tone) {
    using shin::ui::SessionEventTone;
    return shin::ui::tr(tone == SessionEventTone::success ? "Completed" :
        tone == SessionEventTone::error ? "Failed" : tone == SessionEventTone::pending ? "Outcome not recorded" : "Recorded");
}

void render_journal_entry(const shin::ui::SessionActivity& activity) {
    using namespace shin::ui;
    const auto& entry = *journal_entry;
    const auto& event = entry.event;
    action_button(tr("Back"), back_from_journal_entry);
    page_title(journal_operation(event).c_str());
    if (!entry.project.empty()) ImGui::TextColored(muted, "%s", entry.project.c_str());
    const std::array<std::pair<const char*, std::string>, 6> fields{{
        {tr("Time"), event.at_ms > 0 ? clock_label(event.at_ms) : "—"},
        {tr("Agent"), entry.client.empty() ? "—" : entry.client},
        {tr("Reported model"), event.model.empty() ? "—" : event.model},
        {tr("Session"), entry.session.substr(0, 8)},
        {tr("Duration"), event.duration_ms ? number(*event.duration_ms, tr("ms")) : "—"},
        {tr("Result"), journal_outcome(event.tone)}
    }};
    if (ImGui::BeginTable("journal-details", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerH)) {
        for (const auto& [label, value] : fields) {
            ImGui::TableNextColumn(); ImGui::TextColored(muted, "%s", label);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", value.c_str());
        }
        ImGui::EndTable();
    }
    if (!event.error_code.empty()) ImGui::TextWrapped("%s", explain(event.error_code).cause.c_str());
    else if (!event.label.empty()) ImGui::TextWrapped("%s", event.label.c_str());
    for (std::size_t i = 0; i < activity.sessions.size(); ++i) {
        const auto& session = activity.sessions[i];
        if (session.id != entry.session) continue;
        action_button(tr(session_details_open == session.id ? "Hide session details" : "Session details"), journal_session_details);
        if (session_details_open == session.id) {
            session_text(std::string(tr("Tasks: ")) + session.tasks, session.id + "-tasks");
            for (std::size_t line = 0; line < session.details.size(); ++line)
                session_text(session.details[line], session.id + "-" + std::to_string(line), 13, muted);
        }
        break;
    }
}

void session_journal(const shin::ui::SessionActivity& activity, const Json& projects) {
    struct Row { const shin::ui::SessionActivityEntry* session; const shin::ui::SessionActivityEvent* event; };
    std::vector<Row> rows;
    rows.reserve(shin::ui::session_activity_max_journal_events);
    for (const auto& session : activity.sessions)
        for (const auto& event : session.events)
            if (journal_project.empty() || event.project_id == journal_project) rows.push_back({&session, &event});
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.event->at_ms > b.event->at_ms; });
    if (rows.empty()) {
        if (!journal_project.empty()) session_text(shin::ui::tr("No recorded requests for this project."), "journal-empty-project");
        return;
    }
    char limit[128];
    std::snprintf(limit, sizeof(limit), shin::ui::tr("Shown: %zu events · limit %zu"), rows.size(), journal_rows.size());
    session_text(limit, "sessions-journal-limit", 12, muted);
    for (std::size_t i = 0; i < std::min(rows.size(), journal_rows.size()); ++i) {
        const auto& event = *rows[i].event;
        const auto& session = *rows[i].session;
        const auto project = journal_name(projects, event.project_id);
        const bool selectable = !event.key.empty() && std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
            return row.event->key == event.key;
        }) == 1;
        if (selectable) journal_rows[i] = JournalEntry{session.id, project, session.client, event};
        const auto title = journal_operation(event);
        auto detail = event.at_ms > 0 ? clock_label(event.at_ms) : "—";
        if (journal_project.empty() && !project.empty()) detail += " · " + project;
        const auto source = !session.client.empty() ? session.client :
            !event.model.empty() ? event.model : std::string(shin::ui::tr("Session")) + " " + session.id.substr(0, 8);
        detail += " · " + source;
        const auto result = std::string(journal_outcome(event.tone)) + (event.duration_ms ? " · " + number(*event.duration_ms, shin::ui::tr("ms")) : "");
        const float width = ImGui::GetContentRegionAvail().x;
        const float result_width = ImGui::CalcTextSize(result.c_str()).x;
        const auto origin = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        const auto item_key = selectable ? event.key : "unidentified:" + std::to_string(i);
        ImGui::PushID(item_key.c_str());
        ImGui::BeginDisabled(!selectable);
        const bool pressed = ImGui::InvisibleButton("call", {width, 45}, ImGuiButtonFlags_EnableNav);
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
            draw->AddRectFilled(origin, {origin.x + width, origin.y + 45}, ImGui::GetColorU32(hover_surface), 4);
        draw->AddLineH(origin.x, origin.x + width, origin.y + 45, ImGui::GetColorU32(rule));
        draw->AddText({origin.x + 3, origin.y + 4}, ImGui::GetColorU32(ink), clipped(title, width - result_width - 25).c_str());
        ImGui::PushFont(nullptr, 12);
        draw->AddText({origin.x + 3, origin.y + 24}, ImGui::GetColorU32(muted), clipped(detail, width - 10).c_str());
        ImGui::PopFont();
        const auto tint = event.tone == shin::ui::SessionEventTone::error ? red :
            event.tone == shin::ui::SessionEventTone::success ? green : muted;
        draw->AddText({origin.x + width - result_width - 15, origin.y + 4}, ImGui::GetColorU32(tint), result.c_str());
        const auto label = title + " · " + detail + " · " + result;
        if (selectable) {
            draw->AddText({origin.x + width - 10, origin.y + 4}, ImGui::GetColorU32(muted), "›");
            record_action(label, open_journal_record, true, event.key);
        } else session_accessible(label, item_key);
        ImGui::EndDisabled();
        ImGui::PopID();
        if (pressed) queue_panel_action(open_journal_record, event.key);
    }
}

void render_sessions(const ViewState& view, const Json& telemetry) {
    using shin::ui::tr;
    const auto activity = shin::ui::session_activity(telemetry,*view.usage);
    journal_rows.fill(std::nullopt);
    if (journal_entry) { render_journal_entry(activity); return; }
    const int mode = !activity.available ? 0 : activity.sessions.empty() ? 1 : 2;
    if (mode != session_content_mode) { ImGui::SetScrollY(0); session_content_mode = mode; }
    if (!view.connected) session_text(tr("Offline · last received data"), "sessions-offline", 12, amber);
    if (!activity.available) {
        session_text(tr("Session activity is unavailable."), "sessions-unavailable", 16);
        session_text(tr(activity.projection_limited ? "Only part of the session history is available." :
            "Session data will appear when the service provides it."), "sessions-unavailable-help", 13, muted);
    } else {
        if (activity.projection_limited)
            session_text(tr("Only part of the session history is available."), "sessions-projection", 13, amber);
        if (activity.incomplete)
            session_text(tr("MCP history has unobserved updates; totals may be incomplete."), "sessions-partial", 13, amber);
        if (activity.sessions.empty()) {
            session_text(tr("No linked sessions yet"), "sessions-empty", 16);
            session_text(tr("A session appears after an agent sends its session ID with an MCP call. Calls without an ID still count in overall statistics."), "sessions-empty-help", 13, muted);
        }
        session_journal(activity, telemetry.value("project_states", Json::array()));
        if (journal_project.empty()) action_button(tr("Response time details"), 60);
        action_button(tr(session_notes_open ? "Hide recording notes" : "What is recorded?"), 90);
        if (session_notes_open) for (std::size_t i = 0; i < activity.notes.size(); ++i)
            session_text(activity.notes[i], "sessions-note-" + std::to_string(i), 13, muted);
    }
}

void render_activity(const ViewState& view, const Json& telemetry, const Json& history) {
    using shin::ui::tr;
    if (activity_view == 2 && journal_entry) { render_sessions(view, telemetry); return; }
    action_button(tr("Back"), timing_open ? 61 : journal_back);
    page_title(tr(timing_open ? "Response time" : "Journal"));
    if (timing_open) { render_request_timing(view, telemetry); return; }
    const auto scope = journal_name(telemetry.value("project_states", Json::array()), journal_project);
    ImGui::TextColored(muted, "%s", scope.empty() ? tr("All projects") : scope.c_str());
    const auto requests = tr("Shin requests"), tokens = tr("Agent tokens");
    const float tabs_width = ImGui::CalcTextSize(requests).x + ImGui::CalcTextSize(tokens).x
        + 4 * ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x;
    const bool same_line = tabs_width <= ImGui::GetContentRegionAvail().x;
    if (activity_view == 2) primary_button(requests, 102); else action_button(requests, 102);
    if (same_line) ImGui::SameLine();
    if (activity_view == 1) primary_button(tokens, 101); else action_button(tokens, 101);
    if (activity_view == 1) {
        ImGui::TextWrapped("%s", tr("Agent token history is separate from calls to Shin. Totals may be incomplete."));
        render_models(view, telemetry, history);
    } else render_sessions(view, telemetry);
}

bool monitor_homebrew() {
#if defined(__APPLE__)
    static const bool managed = shin::observer::homebrew_managed();
    return managed;
#else
    return false;
#endif
}

const char* installation_label() {
    if (monitor_homebrew()) return "Homebrew";
#if defined(_WIN32)
    return shin::ui::tr("Windows kit");
#else
    return shin::ui::tr("Native macOS kit");
#endif
}

void render_about(const ViewState& view) {
    using namespace shin::ui;
    action_button(tr("Back"), back_from_utility);
    page_title(tr("About Shin"));
    draw_brand(ImGui::GetCursorScreenPos(), 36);
    ImGui::Dummy({36, 36});
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Shin Context Engine");
    ImGui::TextColored(muted, "%s", tr("Local context for coding agents"));
    ImGui::EndGroup();
    const std::array<std::pair<const char*, std::string>, 6> rows{{
        {tr("Monitor"), SHIN_OBSERVER_VERSION},
        {tr("Service"), view.connected ? text_field(view.data, "version", 64) : tr("No response")},
        {"Core", view.connected ? text_field(view.data, "core_version", 64) : "—"},
        {tr("Channel"), "Preview"},
        {tr("Installation"), installation_label()},
        {tr("License"), "Apache-2.0"},
    }};
    version_details = "Shin Context Engine\n";
    for (const auto& [label, value] : rows) version_details += std::string(label) + " " + (value.empty() ? "—" : value) + "\n";
    if (ImGui::BeginTable("about-versions", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerH)) {
        for (const auto& [label, value] : rows) {
            ImGui::TableNextColumn();
            ImGui::TextColored(muted, "%s", label);
            ImGui::TableNextColumn();
            const auto text = value.empty() ? "—" : value.c_str();
            const auto right = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x;
            if (right > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + right);
            ImGui::TextWrapped("%s", text);
            if (label == rows.front().first) session_accessible(text, "monitor-version", 87);
        }
        ImGui::EndTable();
    }
    action_button(tr(version_details_copied ? "Details copied" : "Copy details"), copy_version_details);
    if (ImGui::GetContentRegionAvail().x > 270) ImGui::SameLine();
    action_button(tr("What’s new"), open_release_notes);
    ImGui::SameLine();
    action_button("GitHub", open_repository);
    action_button(tr("Third-party licenses"), open_licenses);
}

void render_settings(const ViewState& view, const Json& telemetry, const Json& history) {
    using shin::ui::tr;
    if (settings_view == SettingsPage::updates) {
        action_button(tr("Back"), back_to_settings);
        page_title(tr("Updates"));
        ImGui::TextColored(muted, "v%s · %s", SHIN_OBSERVER_VERSION, shin::ui::tr("Preview channel"));
        if (monitor_homebrew()) {
            ImGui::TextWrapped("%s", tr("Homebrew installs updates for this package."));
            action_button(tr("How to update with Homebrew"), open_homebrew_guide);
        } else {
            if (!release_update_version.empty()) primary_button(tr("Update available"), 86);
            else primary_button(tr("Check for updates…"), 110);
            action_button(tr("Last update result…"), 111);
        }
        return;
    }
    if (settings_view == SettingsPage::history) {
        action_button(tr("Back"), back_to_settings);
        page_title(tr("Recording history"));
        history_state(history, view.connected, telemetry.value("projection", Json::object()).value("truncated", false));
        ImGui::TextWrapped("%s", tr("Agent token history is separate from calls to Shin. Totals may be incomplete."));
        action_button(tr("Choose agent history…"), 43);
        return;
    }
    action_button(tr("Back"), back_from_utility);
    page_title(tr("Settings"));
    heading(tr("Appearance"));
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const auto label = std::string(language_choices().at(language_preference)) + " ▾";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Language"));
    ImGui::SameLine(right - ImGui::CalcTextSize(label.c_str()).x - 2 * ImGui::GetStyle().FramePadding.x);
    action_button(label, choose_language);
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Compact view"));
    ImGui::SameLine(right - ImGui::GetFrameHeight());
    bool compact = platform_compact_view();
    if (ImGui::Checkbox("##compact-view", &compact)) queue_panel_action(compact_view);
    record_action(tr("Compact view"), compact_view);
    ImGui::Separator();
    heading(tr("System settings"));
    option_row(tr("Start at login"), 112);
    option_row(tr("Updates"), update_settings, installation_label());
    option_row(tr("Storage"), 83);
    option_row(tr("Recording history"), history_settings);
}
